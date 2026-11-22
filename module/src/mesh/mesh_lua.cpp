#include "mesh/mesh_lua.h"

#include <string>

#include "core/core.h"
#include "core/lua_util.h"
#include "engine/engine_guard.h"
#include "mesh/mesh_worker.h"

using namespace GarrysMod::Lua;

namespace rcx
{
	bool IsFeatureOff(ILuaBase* lua, const char* feature, std::string& reason);
}

/*
	rcx.mesh for Lua: Build queues a merge (the part files' bytes, read by Lua from the game's own mounts), Poll hands back
	finished ones, Cancel drops one, Exists asks whether a model is already on disk, Stats counts the queue. Every field
	is type- and size-checked; a bad job is refused with a reason before anything is queued. Build needs mesh_combine on
	(the build check, the kill switches) and arms its crash latch on first use.
*/
namespace rcx::mesh
{
	namespace
	{
		constexpr const char* kFeature = "mesh_combine";
		constexpr size_t kMaxShortString = 260;
		constexpr size_t kMaxMaterials = 64;
		constexpr size_t kMaxBodygroups = 64;
		constexpr size_t kMaxBake = 1024;
		constexpr uint64_t kMaxDiskBytes = 512ull * 1024 * 1024;

		bool prepared = false;

		// The string at the top of the stack, or false when it isn't one or is longer than maxBytes.
		bool TopString(ILuaBase* lua, size_t maxBytes, std::string& out)
		{
			if (!lua->IsType(-1, Type::String))
			{
				return false;
			}

			unsigned int length = 0;
			const char* text = lua->GetString(-1, &length);

			if (text == nullptr || length > maxBytes)
			{
				return false;
			}

			out.assign(text, length);
			return true;
		}

		bool FieldString(ILuaBase* lua, int table, const char* key, size_t maxBytes, std::string& out, bool optional = false)
		{
			lua->GetField(table, key);
			bool missing = lua->IsType(-1, Type::Nil);
			bool ok = (optional && missing) || TopString(lua, maxBytes, out);
			lua->Pop();
			return ok;
		}

		bool FieldBytes(ILuaBase* lua, int table, const char* key, Bytes& out)
		{
			std::string text;

			if (!FieldString(lua, table, key, kMaxFileBytes, text))
			{
				return false;
			}

			out.assign(text.begin(), text.end());
			return true;
		}

		bool FieldNumber(ILuaBase* lua, int table, const char* key, double minimum, double maximum, double& out, bool optional = true)
		{
			lua->GetField(table, key);
			bool missing = lua->IsType(-1, Type::Nil);
			bool ok = (optional && missing) || rcx::lua::ReadNumber(lua, -1, minimum, maximum, out);
			lua->Pop();
			return ok;
		}

		bool FieldVector(ILuaBase* lua, int table, const char* key, double out[3])
		{
			lua->GetField(table, key);

			if (lua->IsType(-1, Type::Nil))
			{
				lua->Pop();
				return true;
			}

			if (!lua->IsType(-1, Type::Table))
			{
				lua->Pop();
				return false;
			}

			int vector = lua->Top();
			bool ok = true;

			for (int k = 0; k < 3 && ok; ++k)
			{
				lua->PushNumber(k + 1);
				lua->GetTable(vector);
				ok = rcx::lua::ReadNumber(lua, -1, -1e6, 1e6, out[k]);
				lua->Pop();
			}

			lua->Pop();
			return ok;
		}

		bool ReadPart(ILuaBase* lua, int part, PartInput& out, std::string& reason)
		{
			if (!FieldBytes(lua, part, "mdl", out.mdl) || !FieldBytes(lua, part, "vvd", out.vvd) || !FieldBytes(lua, part, "vtx", out.vtx))
			{
				reason = "a part needs mdl, vvd and vtx strings";
				return false;
			}

			double skin = 0;

			if (!FieldNumber(lua, part, "skin", 0, 255, skin) || !FieldString(lua, part, "material", kMaxShortString, out.material, true))
			{
				reason = "a part's skin or material is out of range";
				return false;
			}

			out.skin = static_cast<int>(skin);

			lua->GetField(part, "materials");

			if (lua->IsType(-1, Type::Table))
			{
				int list = lua->Top();

				for (size_t k = 1; k <= kMaxMaterials; ++k)
				{
					lua->PushNumber(static_cast<double>(k));
					lua->GetTable(list);

					if (lua->IsType(-1, Type::Nil))
					{
						lua->Pop();
						break;
					}

					std::string material;

					if (!TopString(lua, kMaxShortString, material))
					{
						lua->Pop(2);
						reason = "a part's materials must be strings";
						return false;
					}

					out.materials.push_back(material);
					lua->Pop();
				}
			}

			lua->Pop();

			lua->GetField(part, "bodygroups");

			if (lua->IsType(-1, Type::Table))
			{
				int groups = lua->Top();
				lua->PushNil();

				while (lua->Next(groups) != 0)
				{
					double value = 0;

					if (out.bodygroups.size() >= kMaxBodygroups || !lua->IsType(-2, Type::String) || !rcx::lua::ReadNumber(lua, -1, 0, 255, value))
					{
						lua->Pop(3);
						reason = "a part's bodygroups must map names to numbers";
						return false;
					}

					unsigned int length = 0;
					const char* name = lua->GetString(-2, &length);
					out.bodygroups.emplace_back(std::string(name, std::min<unsigned int>(length, 64)), static_cast<int>(value));
					lua->Pop();
				}
			}

			lua->Pop();

			lua->GetField(part, "positioned");
			out.positioned = lua->IsType(-1, Type::Bool) && lua->GetBool(-1);
			lua->Pop();

			if (out.positioned)
			{
				if (!FieldString(lua, part, "bone", 64, out.placement.bone, true) || !FieldString(lua, part, "attachment", 64, out.placement.attachment, true) ||
					!FieldVector(lua, part, "pos", out.placement.pos) || !FieldVector(lua, part, "ang", out.placement.ang) ||
					!FieldNumber(lua, part, "scale", 0.01, 100, out.placement.scale))
				{
					reason = "a positioned part's bone, attachment, pos, ang or scale is out of range";
					return false;
				}
			}

			return true;
		}

		RCX_FUNCTION(BuildLua)
		{
			std::string reason;

			if (rcx::IsFeatureOff(lua, kFeature, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			if (!lua->IsType(1, Type::Table))
			{
				return rcx::lua::PushFailure(lua, "Build takes a job table");
			}

			MergeInput input;

			if (!FieldString(lua, 1, "name", 32, input.name) || !FieldBytes(lua, 1, "body", input.body))
			{
				return rcx::lua::PushFailure(lua, "a job needs a name (32 hex digits) and the body model's bytes");
			}

			lua->GetField(1, "parts");

			if (!lua->IsType(-1, Type::Table))
			{
				lua->Pop();
				return rcx::lua::PushFailure(lua, "a job needs a parts list");
			}

			int parts = lua->Top();

			for (size_t k = 1; k <= kMaxParts + 1; ++k)
			{
				lua->PushNumber(static_cast<double>(k));
				lua->GetTable(parts);

				if (lua->IsType(-1, Type::Nil))
				{
					lua->Pop();
					break;
				}

				if (k > kMaxParts || !lua->IsType(-1, Type::Table))
				{
					lua->Pop(2);
					return rcx::lua::PushFailure(lua, "the parts list is too long or holds a non-table");
				}

				PartInput part;

				if (!ReadPart(lua, lua->Top(), part, reason))
				{
					lua->Pop(2);
					return rcx::lua::PushFailure(lua, reason);
				}

				input.parts.push_back(std::move(part));
				lua->Pop();
			}

			lua->Pop();

			lua->GetField(1, "bake");

			if (lua->IsType(-1, Type::Table))
			{
				int bake = lua->Top();
				lua->PushNil();

				while (lua->Next(bake) != 0)
				{
					std::string target;

					if (input.bake.size() >= kMaxBake || !lua->IsType(-2, Type::String) || !TopString(lua, 64, target))
					{
						lua->Pop(3);
						return rcx::lua::PushFailure(lua, "bake must map bone names to bone names");
					}

					unsigned int length = 0;
					const char* name = lua->GetString(-2, &length);
					input.bake.emplace_back(std::string(name, std::min<unsigned int>(length, 64)), target);
					lua->Pop();
				}
			}

			lua->Pop();

			if (input.parts.empty())
			{
				return rcx::lua::PushFailure(lua, "a job needs at least one part");
			}

			if (!prepared)
			{
				prepared = true;
				rcx::engine::Arm(kFeature);
				PruneFolder(kMaxDiskBytes);
			}

			uint32_t id = 0;

			if (!Submit(std::move(input), id, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushNumber(id);
			return 1;
		}

		void SetNumber(ILuaBase* lua, const char* name, double value)
		{
			lua->PushNumber(value);
			lua->SetField(-2, name);
		}

		RCX_FUNCTION(PollLua)
		{
			std::vector<BuildResult> results = Poll();
			lua->CreateTable();
			double index = 1;

			for (const BuildResult& result : results)
			{
				lua->PushNumber(index);
				lua->CreateTable();
				SetNumber(lua, "id", result.id);
				lua->PushBool(result.ok);
				lua->SetField(-2, "ok");
				SetNumber(lua, "ms", result.milliseconds);

				if (result.ok)
				{
					std::string path = "models/rcx_merged/" + result.name + ".mdl";
					lua->PushString(path.c_str());
					lua->SetField(-2, "path");
					SetNumber(lua, "bones", result.stats.bones);
					SetNumber(lua, "baked", result.stats.baked);
					SetNumber(lua, "rebinds", result.stats.rebinds);
					SetNumber(lua, "meshes", result.stats.meshes);
					SetNumber(lua, "vertices", result.stats.vertices);
				}
				else
				{
					lua->PushString(result.error.c_str());
					lua->SetField(-2, "error");
				}

				lua->SetTable(-3);
				index += 1;
			}

			return 1;
		}

		RCX_FUNCTION(CancelLua)
		{
			int64_t id = 0;

			if (!rcx::lua::ReadInteger(lua, 1, 1, 0xffffffffll, id))
			{
				return rcx::lua::PushFailure(lua, "Cancel takes a build id");
			}

			lua->PushBool(Cancel(static_cast<uint32_t>(id)));
			return 1;
		}

		RCX_FUNCTION(ExistsLua)
		{
			if (!lua->IsType(1, Type::String))
			{
				lua->PushBool(false);
				return 1;
			}

			unsigned int length = 0;
			const char* name = lua->GetString(1, &length);
			lua->PushBool(Exists(std::string(name, std::min<unsigned int>(length, 64))));
			return 1;
		}

		RCX_FUNCTION(StatsLua)
		{
			QueueStats stats = GetStats();
			lua->CreateTable();
			SetNumber(lua, "queued", stats.queued);
			lua->PushBool(stats.building);
			lua->SetField(-2, "building");
			SetNumber(lua, "done", static_cast<double>(stats.done));
			SetNumber(lua, "failed", static_cast<double>(stats.failed));
			SetNumber(lua, "canceled", static_cast<double>(stats.canceled));
			return 1;
		}

		void SetFunction(ILuaBase* lua, const char* name, CFunc function)
		{
			lua->PushCFunction(function);
			lua->SetField(-2, name);
		}
	}

	void PushTable(ILuaBase* lua)
	{
		lua->CreateTable();
		SetFunction(lua, "Build", BuildLua);
		SetFunction(lua, "Poll", PollLua);
		SetFunction(lua, "Cancel", CancelLua);
		SetFunction(lua, "Exists", ExistsLua);
		SetFunction(lua, "Stats", StatsLua);
	}

	void OnLuaClose()
	{
		Shutdown();
		prepared = false;
	}
}
