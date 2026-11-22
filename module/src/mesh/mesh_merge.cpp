#include "mesh/mesh_merge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

/*
	A line-for-line port of tools/mesh_prototype/merge.py (with bake_plan.py's bake map and merge_cases.py's placement of
	positioned parts), kept byte-identical to it: the golden test (tests/mesh_merge_test.cpp) compares the two outputs.
	Matrix maths is in double, in the prototype's order of operations, and only packed to float at the end.
*/
namespace rcx::mesh
{
	namespace
	{
		constexpr int kBoneSize = 216;
		constexpr int kModelSize = 148;
		constexpr int kMeshSize = 116;
		constexpr int kTextureSize = 64;
		constexpr int kVertexSize = 48;
		constexpr int kTangentSize = 16;
		constexpr int kHeaderSize = 408;
		constexpr int kHeader2Size = 256;

		using M34 = std::array<std::array<double, 4>, 3>;

		struct Failure : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		// Bounds-checked reads from a file's bytes.
		struct View
		{
			const Bytes& bytes;
			const char* what;

			void Need(int64_t offset, int64_t size) const
			{
				if (offset < 0 || size < 0 || offset + size > static_cast<int64_t>(bytes.size()))
				{
					throw Failure(std::string(what) + ": an offset out of the file");
				}
			}

			int32_t I32(int64_t offset) const
			{
				Need(offset, 4);
				int32_t value = 0;
				std::memcpy(&value, bytes.data() + offset, 4);
				return value;
			}

			int16_t I16(int64_t offset) const
			{
				Need(offset, 2);
				int16_t value = 0;
				std::memcpy(&value, bytes.data() + offset, 2);
				return value;
			}

			uint8_t U8(int64_t offset) const
			{
				Need(offset, 1);
				return bytes[static_cast<size_t>(offset)];
			}

			double F32(int64_t offset) const
			{
				Need(offset, 4);
				float value = 0;
				std::memcpy(&value, bytes.data() + offset, 4);
				return value;
			}

			std::string CStr(int64_t offset) const
			{
				Need(offset, 1);
				const uint8_t* begin = bytes.data() + offset;
				const uint8_t* end = static_cast<const uint8_t*>(std::memchr(begin, 0, bytes.size() - static_cast<size_t>(offset)));

				if (end == nullptr || end - begin > 1024)
				{
					throw Failure(std::string(what) + ": an unterminated string");
				}

				return std::string(reinterpret_cast<const char*>(begin), static_cast<size_t>(end - begin));
			}

			Bytes Slice(int64_t offset, int64_t size) const
			{
				Need(offset, size);
				return Bytes(bytes.begin() + offset, bytes.begin() + offset + size);
			}
		};

		std::string Lower(std::string text)
		{
			for (char& c : text)
			{
				if (c >= 'A' && c <= 'Z')
				{
					c = static_cast<char>(c - 'A' + 'a');
				}
			}

			return text;
		}

		void PutI32(Bytes& data, size_t offset, int32_t value)
		{
			std::memcpy(data.data() + offset, &value, 4);
		}

		void PutI16(Bytes& data, size_t offset, int16_t value)
		{
			std::memcpy(data.data() + offset, &value, 2);
		}

		void PutF32(Bytes& data, size_t offset, double value)
		{
			float narrow = static_cast<float>(value);
			std::memcpy(data.data() + offset, &narrow, 4);
		}

		void AppendI32(Bytes& data, int32_t value)
		{
			size_t at = data.size();
			data.resize(at + 4);
			PutI32(data, at, value);
		}

		void AppendF32(Bytes& data, double value)
		{
			size_t at = data.size();
			data.resize(at + 4);
			PutF32(data, at, value);
		}

		M34 MatrixAt(const Bytes& raw, size_t offset)
		{
			M34 m{};

			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 4; ++j)
				{
					float value = 0;
					std::memcpy(&value, raw.data() + offset + (i * 4 + j) * 4, 4);
					m[i][j] = value;
				}
			}

			return m;
		}

		M34 PoseToBone(const Bytes& raw)
		{
			return MatrixAt(raw, 96);
		}

		M34 Mul(const M34& a, const M34& b)
		{
			M34 out{};

			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 4; ++j)
				{
					double sum = 0.0;

					for (int k = 0; k < 3; ++k)
					{
						sum += a[i][k] * b[k][j];
					}

					out[i][j] = sum + (j == 3 ? a[i][3] : 0.0);
				}
			}

			return out;
		}

		M34 Inverse(const M34& m)
		{
			M34 out{};

			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
				{
					out[i][j] = m[j][i];
				}
			}

			for (int i = 0; i < 3; ++i)
			{
				double sum = 0.0;

				for (int k = 0; k < 3; ++k)
				{
					sum += out[i][k] * m[k][3];
				}

				out[i][3] = -sum;
			}

			return out;
		}

		M34 QuatMatrix(double x, double y, double z, double w, double px, double py, double pz)
		{
			M34 m{};
			m[0] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), px};
			m[1] = {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), py};
			m[2] = {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), pz};
			return m;
		}

		M34 QuatMatrix4(const double q[4], const double p[3])
		{
			return QuatMatrix(q[0], q[1], q[2], q[3], p[0], p[1], p[2]);
		}

		void EulerQuat(double x, double y, double z, double out[4])
		{
			double sr = std::sin(x * 0.5), cr = std::cos(x * 0.5);
			double sp = std::sin(y * 0.5), cp = std::cos(y * 0.5);
			double sy = std::sin(z * 0.5), cy = std::cos(z * 0.5);
			double srCp = sr * cp, crSp = cr * sp;
			double crCp = cr * cp, srSp = sr * sp;
			out[0] = srCp * cy - crSp * sy;
			out[1] = crSp * cy + srCp * sy;
			out[2] = crCp * sy - srSp * cy;
			out[3] = crCp * cy + srSp * sy;
		}

		void Quat64(uint64_t bits, double out[4])
		{
			double x = (static_cast<double>(bits & 0x1FFFFF) - 1048576) / 1048576.5;
			double y = (static_cast<double>((bits >> 21) & 0x1FFFFF) - 1048576) / 1048576.5;
			double z = (static_cast<double>((bits >> 42) & 0x1FFFFF) - 1048576) / 1048576.5;
			double w = std::sqrt(std::max(0.0, 1.0 - x * x - y * y - z * z));
			out[0] = x;
			out[1] = y;
			out[2] = z;
			out[3] = (bits >> 63) != 0 ? -w : w;
		}

		void Quat48(uint16_t a, uint16_t b, uint16_t c, double out[4])
		{
			double x = (static_cast<double>(a) - 32768) / 32768.0;
			double y = (static_cast<double>(b) - 32768) / 32768.0;
			double z = (static_cast<double>(c & 0x7FFF) - 16384) / 16384.0;
			double w = std::sqrt(std::max(0.0, 1.0 - x * x - y * y - z * z));
			out[0] = x;
			out[1] = y;
			out[2] = z;
			out[3] = (c & 0x8000) != 0 ? -w : w;
		}

		double HalfFloat(uint16_t h)
		{
			int sign = (h >> 15) & 1;
			int exponent = (h >> 10) & 0x1F;
			int mantissa = h & 0x3FF;
			double value = 0.0;

			if (exponent == 0)
			{
				value = std::ldexp(static_cast<double>(mantissa), -24);
			}
			else if (exponent == 31)
			{
				value = mantissa != 0 ? std::nan("") : HUGE_VAL;
			}
			else
			{
				value = std::ldexp(static_cast<double>(mantissa + 1024), exponent - 25);
			}

			return sign != 0 ? -value : value;
		}

		M34 BoneLocal(const Bytes& raw)
		{
			float q[4]{};
			float p[3]{};
			std::memcpy(q, raw.data() + 44, 16);
			std::memcpy(p, raw.data() + 32, 12);
			return QuatMatrix(q[0], q[1], q[2], q[3], p[0], p[1], p[2]);
		}

		M34 AngleMatrix(double pitch, double yaw, double roll, const double pos[3])
		{
			constexpr double kRadians = 3.14159265358979323846 / 180.0;
			double sp = std::sin(pitch * kRadians), cp = std::cos(pitch * kRadians);
			double sy = std::sin(yaw * kRadians), cy = std::cos(yaw * kRadians);
			double sr = std::sin(roll * kRadians), cr = std::cos(roll * kRadians);
			M34 m{};
			m[0] = {cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy, pos[0]};
			m[1] = {cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy, pos[1]};
			m[2] = {-sp, sr * cp, cr * cp, pos[2]};
			return m;
		}

		void Rotate(const M34& m, const double v[3], double out[3])
		{
			for (int i = 0; i < 3; ++i)
			{
				out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
			}
		}

		double MaxDifference(const M34& a, const M34& b)
		{
			double worst = 0.0;

			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 4; ++j)
				{
					worst = std::max(worst, std::abs(a[i][j] - b[i][j]));
				}
			}

			return worst;
		}

		struct Bone
		{
			std::string name;
			Bytes raw;
			int parent = -1;
		};

		struct Mesh
		{
			Bytes raw;
			std::vector<Bytes> verts;
			std::vector<Bytes> tans;
			int texture = 0;
			int64_t vtx = 0;
			int material = 0;
			int part = 0;
			int offset = 0;
		};

		struct Part
		{
			const PartInput* input = nullptr;
			View m;
			View v;
			View x;
			int numbones = 0;
			int boneindex = 0;
			int numtextures = 0;
			int textureindex = 0;
			int numcdtextures = 0;
			int cdtextureindex = 0;
			int numskinref = 0;
			int numskinfamilies = 0;
			int skinindex = 0;
			int numbodyparts = 0;
			int bodypartindex = 0;
			int flags = 0;
			double vectors[18]{};
			std::vector<Bone> bones;
			std::vector<std::string> textures;
			std::vector<std::string> cds;
			std::vector<int> skins;
			std::vector<Bytes> lodVerts;
			std::vector<Bytes> lodTans;
			std::vector<M34> rest;
			std::vector<M34> pose;
			bool positioned = false;
			std::string rigidBoneName;
			Bytes rigidBoneRaw;
			M34 rigidMatrix{};

			explicit Part(const PartInput& part)
				: input(&part), m{part.mdl, "part mdl"}, v{part.vvd, "part vvd"}, x{part.vtx, "part vtx"}
			{
				if (part.mdl.size() < kHeaderSize || std::memcmp(part.mdl.data(), "IDST", 4) != 0 || m.I32(4) != 48)
				{
					throw Failure("not an MDL v48");
				}

				if (part.vvd.size() < 64 || std::memcmp(part.vvd.data(), "IDSV", 4) != 0 || v.I32(4) != 4)
				{
					throw Failure("not a VVD v4");
				}

				if (x.I32(0) != 7)
				{
					throw Failure("not a VTX v7");
				}

				auto h = [&](int k) { return m.I32(156 + 4 * k); };
				numbones = h(0);
				boneindex = h(1);
				numtextures = h(12);
				textureindex = h(13);
				numcdtextures = h(14);
				cdtextureindex = h(15);
				numskinref = h(16);
				numskinfamilies = h(17);
				skinindex = h(18);
				numbodyparts = h(19);
				bodypartindex = h(20);
				int numflexdesc = h(26);
				int numincludemodels = h(45);
				flags = m.I32(152);

				if (numbones < 0 || numbones > kMaxBones || numtextures < 0 || numtextures > 256 || numcdtextures < 0 || numcdtextures > 64 ||
					numskinref < 0 || numskinref > 256 || numskinfamilies < 0 || numskinfamilies > 64 || numbodyparts < 0 || numbodyparts > 64)
				{
					throw Failure("a count out of range");
				}

				for (int k = 0; k < 18; ++k)
				{
					vectors[k] = m.F32(80 + 4 * k);
				}

				for (int b = 0; b < numbones; ++b)
				{
					int64_t o = boneindex + static_cast<int64_t>(b) * kBoneSize;
					Bone bone;
					bone.raw = m.Slice(o, kBoneSize);
					bone.name = m.CStr(o + m.I32(o));
					bone.parent = m.I32(o + 4);

					if (bone.parent >= b || bone.parent < -1)
					{
						throw Failure("a bone before its parent");
					}

					if (m.I32(o + 164) != 0)
					{
						throw Failure("a part with procedural bones");
					}

					bones.push_back(std::move(bone));
				}

				if (numflexdesc != 0 || numincludemodels != 0)
				{
					throw Failure("a part with flexes or included models");
				}

				for (int t = 0; t < numtextures; ++t)
				{
					int64_t o = textureindex + static_cast<int64_t>(t) * kTextureSize;
					textures.push_back(m.CStr(o + m.I32(o)));
				}

				for (int c = 0; c < numcdtextures; ++c)
				{
					cds.push_back(m.CStr(m.I32(cdtextureindex + 4 * c)));
				}

				for (int k = 0; k < numskinref * numskinfamilies; ++k)
				{
					skins.push_back(m.I16(skinindex + 2 * k));
				}

				LoadLod0();

				// rest: the bone structs' pose, what a bonemerged part's own bones take; pose: frame 0 of the first
				// animation, what a part placed on a bone or attachment is drawn in
				std::vector<M34> locals = AnimLocals();

				for (size_t index = 0; index < bones.size(); ++index)
				{
					const Bone& bone = bones[index];
					M34 local = BoneLocal(bone.raw);
					rest.push_back(bone.parent >= 0 ? Mul(rest[static_cast<size_t>(bone.parent)], local) : local);
					pose.push_back(bone.parent >= 0 ? Mul(pose[static_cast<size_t>(bone.parent)], locals[index]) : locals[index]);
				}
			}

			double FirstFrameValue(int64_t valueptr, int k) const
			{
				int offset = m.I16(valueptr + 2 * k);

				if (offset == 0)
				{
					return 0.0;
				}

				int64_t track = valueptr + offset;
				return m.U8(track) > 0 ? static_cast<double>(m.I16(track + 2)) : 0.0;
			}

			// Each bone's local transform in frame 0 of the first animation, which the engine draws a lone part in; the
			// bone struct's where the animation has no data for it (or is external or a delta).
			std::vector<M34> AnimLocals() const
			{
				std::vector<M34> locals;

				for (const Bone& bone : bones)
				{
					locals.push_back(BoneLocal(bone.raw));
				}

				int numanim = m.I32(156 + 4 * 6);
				int64_t animindex = m.I32(156 + 4 * 7);

				if (numanim < 1 || animindex <= 0 || m.I32(animindex + 52) != 0)
				{
					return locals;
				}

				int64_t ptr = 0;

				if (m.I32(animindex + 84) != 0)
				{
					ptr = animindex + m.I32(animindex + m.I32(animindex + 80) + 4);
				}
				else
				{
					ptr = animindex + m.I32(animindex + 56);
				}

				for (size_t guard = 0; guard <= bones.size(); ++guard)
				{
					int bone = m.U8(ptr);
					int animFlags = m.U8(ptr + 1);
					int next = m.I16(ptr + 2);

					if (bone >= static_cast<int>(bones.size()))
					{
						break;
					}

					const Bytes& raw = bones[static_cast<size_t>(bone)].raw;
					double quat[4]{};
					double pos[3]{};

					for (int k = 0; k < 4; ++k)
					{
						float value = 0;
						std::memcpy(&value, raw.data() + 44 + 4 * k, 4);
						quat[k] = value;
					}

					for (int k = 0; k < 3; ++k)
					{
						float value = 0;
						std::memcpy(&value, raw.data() + 32 + 4 * k, 4);
						pos[k] = value;
					}

					int64_t data = ptr + 4;

					if ((animFlags & 0x01) != 0)
					{
						Quat48(static_cast<uint16_t>(m.I16(data)), static_cast<uint16_t>(m.I16(data + 2)), static_cast<uint16_t>(m.I16(data + 4)), quat);
						data += 6;
					}

					if ((animFlags & 0x20) != 0)
					{
						m.Need(data, 8);
						uint64_t bits = 0;
						std::memcpy(&bits, m.bytes.data() + data, 8);
						Quat64(bits, quat);
						data += 8;
					}

					if ((animFlags & 0x02) != 0)
					{
						for (int k = 0; k < 3; ++k)
						{
							pos[k] = HalfFloat(static_cast<uint16_t>(m.I16(data + 2 * k)));
						}

						data += 6;
					}

					if ((animFlags & 0x04) != 0)
					{
						double euler[3]{};

						for (int k = 0; k < 3; ++k)
						{
							float rot = 0;
							float scale = 0;
							std::memcpy(&rot, raw.data() + 60 + 4 * k, 4);
							std::memcpy(&scale, raw.data() + 84 + 4 * k, 4);
							euler[k] = rot + FirstFrameValue(data, k) * scale;
						}

						EulerQuat(euler[0], euler[1], euler[2], quat);
						data += 6;
					}

					if ((animFlags & 0x08) != 0)
					{
						for (int k = 0; k < 3; ++k)
						{
							float base = 0;
							float scale = 0;
							std::memcpy(&base, raw.data() + 32 + 4 * k, 4);
							std::memcpy(&scale, raw.data() + 72 + 4 * k, 4);
							pos[k] = base + FirstFrameValue(data, k) * scale;
						}
					}

					if ((animFlags & 0x10) == 0)
					{
						locals[static_cast<size_t>(bone)] = QuatMatrix4(quat, pos);
					}

					if (next == 0)
					{
						break;
					}

					ptr += next;
				}

				return locals;
			}

			void LoadLod0()
			{
				int numfix = v.I32(48);
				int fixstart = v.I32(52);
				int vstart = v.I32(56);
				int tstart = v.I32(60);
				int count = v.I32(16);

				if (tstart < vstart || numfix < 0 || numfix > 4096)
				{
					throw Failure("a malformed VVD header");
				}

				int total = (tstart - vstart) / kVertexSize;

				if (total > kMaxVertices)
				{
					throw Failure("too many vertices");
				}

				std::vector<Bytes> verts;
				std::vector<Bytes> tans;

				for (int k = 0; k < total; ++k)
				{
					verts.push_back(v.Slice(vstart + static_cast<int64_t>(k) * kVertexSize, kVertexSize));
					tans.push_back(v.Slice(tstart + static_cast<int64_t>(k) * kTangentSize, kTangentSize));
				}

				if (numfix == 0)
				{
					if (count < 0 || count > total)
					{
						throw Failure("a VVD vertex count out of range");
					}

					lodVerts.assign(verts.begin(), verts.begin() + count);
					lodTans.assign(tans.begin(), tans.begin() + count);
					return;
				}

				for (int k = 0; k < numfix; ++k)
				{
					int lod = v.I32(fixstart + 12 * k);
					int source = v.I32(fixstart + 12 * k + 4);
					int n = v.I32(fixstart + 12 * k + 8);

					if (lod < 0)
					{
						continue;
					}

					if (source < 0 || n < 0 || source + n > total)
					{
						throw Failure("a VVD fixup out of range");
					}

					lodVerts.insert(lodVerts.end(), verts.begin() + source, verts.begin() + source + n);
					lodTans.insert(lodTans.end(), tans.begin() + source, tans.begin() + source + n);
				}
			}

			std::string MaterialPath(int texture) const
			{
				if (!input->material.empty())
				{
					return input->material;
				}

				if (!input->materials.empty() && texture < static_cast<int>(input->materials.size()))
				{
					return input->materials[static_cast<size_t>(texture)];
				}

				if (texture < 0 || texture >= static_cast<int>(textures.size()))
				{
					throw Failure("a texture index out of range");
				}

				std::string path = (cds.empty() ? std::string() : cds[0]) + textures[static_cast<size_t>(texture)];
				std::replace(path.begin(), path.end(), '/', '\\');
				return path;
			}

			std::vector<Mesh> ChosenMeshes() const
			{
				std::vector<Mesh> out;
				int skin = input->skin;

				if (skin >= numskinfamilies)
				{
					skin = 0;
				}

				int64_t vtxBodyparts = x.I32(32);

				for (int b = 0; b < numbodyparts; ++b)
				{
					int64_t bp = bodypartindex + static_cast<int64_t>(b) * 16;
					std::string name = m.CStr(bp + m.I32(bp));
					int nummodels = m.I32(bp + 4);
					int64_t modelindex = m.I32(bp + 12);
					int choice = 0;

					for (const auto& group : input->bodygroups)
					{
						if (group.first == name)
						{
							choice = group.second;
							break;
						}
					}

					if (!(choice >= 0 && choice < nummodels))
					{
						choice = 0;
					}

					int64_t mo = bp + modelindex + static_cast<int64_t>(choice) * kModelSize;
					int nummeshes = m.I32(mo + 72);
					int64_t meshindex = m.I32(mo + 76);
					int vertexindex = m.I32(mo + 84);
					int tangentsindex = m.I32(mo + 88);
					int baseV = vertexindex / kVertexSize;
					int baseT = tangentsindex / kTangentSize;
					int64_t xbp = vtxBodyparts + static_cast<int64_t>(b) * 8;
					int64_t xmodel = xbp + x.I32(xbp + 4) + static_cast<int64_t>(choice) * 8;
					int64_t xlod = xmodel + x.I32(xmodel + 4);
					int64_t xmeshes = xlod + x.I32(xlod + 4);

					if (nummeshes < 0 || nummeshes > 256)
					{
						throw Failure("a mesh count out of range");
					}

					for (int k = 0; k < nummeshes; ++k)
					{
						int64_t me = mo + meshindex + static_cast<int64_t>(k) * kMeshSize;
						int material = m.I32(me);
						int meshVerts = m.I32(me + 8);
						int offset = m.I32(me + 12);

						if (m.I32(me + 16) != 0)
						{
							throw Failure("a mesh with flexes");
						}

						int64_t skinSlot = static_cast<int64_t>(skin) * numskinref + material;

						if (material < 0 || skinSlot < 0 || skinSlot >= static_cast<int64_t>(skins.size()))
						{
							throw Failure("a mesh material out of the skin table");
						}

						int64_t firstV = static_cast<int64_t>(baseV) + offset;
						int64_t firstT = static_cast<int64_t>(baseT) + offset;

						if (meshVerts < 0 || firstV < 0 || firstT < 0 || firstV + meshVerts > static_cast<int64_t>(lodVerts.size()) ||
							firstT + meshVerts > static_cast<int64_t>(lodTans.size()))
						{
							throw Failure("vertex range out of the VVD");
						}

						Mesh mesh;
						mesh.raw = m.Slice(me, kMeshSize);
						mesh.verts.assign(lodVerts.begin() + firstV, lodVerts.begin() + firstV + meshVerts);
						mesh.tans.assign(lodTans.begin() + firstT, lodTans.begin() + firstT + meshVerts);
						mesh.texture = skins[static_cast<size_t>(skinSlot)];
						mesh.vtx = xmeshes + static_cast<int64_t>(k) * 9;
						out.push_back(std::move(mesh));
					}
				}

				return out;
			}
		};

		// The body's bones (lowercase name to struct, index, parent) and attachments, and each bone's rest transform.
		struct Body
		{
			struct BodyBone
			{
				std::string name;
				Bytes raw;
				int index = 0;
				int parent = -1;
			};

			struct Attachment
			{
				std::string bone;
				M34 local{};
			};

			std::map<std::string, BodyBone> bones;
			std::map<std::string, Attachment> attachments;
			std::map<std::string, M34> rest;

			explicit Body(const Bytes& mdl)
			{
				View m{mdl, "body mdl"};

				if (mdl.size() < kHeaderSize || std::memcmp(mdl.data(), "IDST", 4) != 0)
				{
					throw Failure("the body isn't an MDL");
				}

				int numbones = m.I32(156);
				int boneindex = m.I32(160);

				if (numbones < 0 || numbones > kMaxBones)
				{
					throw Failure("the body's bone count is out of range");
				}

				std::vector<std::string> names;
				std::vector<M34> byIndex;

				for (int b = 0; b < numbones; ++b)
				{
					int64_t o = boneindex + static_cast<int64_t>(b) * kBoneSize;
					BodyBone bone;
					bone.name = m.CStr(o + m.I32(o));
					bone.raw = m.Slice(o, kBoneSize);
					bone.index = b;
					bone.parent = m.I32(o + 4);

					if (bone.parent >= b || bone.parent < -1)
					{
						throw Failure("a body bone before its parent");
					}

					M34 local = BoneLocal(bone.raw);
					byIndex.push_back(bone.parent >= 0 ? Mul(byIndex[static_cast<size_t>(bone.parent)], local) : local);
					names.push_back(bone.name);
					std::string key = Lower(bone.name);
					rest[key] = byIndex.back();
					bones[key] = std::move(bone);
				}

				int numatt = m.I32(156 + 4 * 21);
				int attindex = m.I32(156 + 4 * 22);

				if (numatt < 0 || numatt > 512)
				{
					throw Failure("the body's attachment count is out of range");
				}

				for (int a = 0; a < numatt; ++a)
				{
					int64_t o = attindex + static_cast<int64_t>(a) * 92;
					std::string name = m.CStr(o + m.I32(o));
					int bone = m.I32(o + 8);

					if (bone < 0 || bone >= numbones)
					{
						throw Failure("an attachment on a missing bone");
					}

					Attachment attachment;
					attachment.bone = names[static_cast<size_t>(bone)];
					m.Need(o + 12, 48);
					attachment.local = MatrixAt(mdl, static_cast<size_t>(o + 12));
					attachments[Lower(name)] = attachment;
				}
			}
		};

		struct MergedBone
		{
			std::string name;
			Bytes raw;
			int parent = -1;
			int flags = 0;
		};

		// The prototype's Writer: strings are written after everything else, each once, at its first use's place in the order.
		struct Writer
		{
			Bytes data;
			struct Ref
			{
				size_t owner;
				size_t field;
				std::string text;
			};
			std::vector<Ref> strings;

			size_t Add(const Bytes& raw)
			{
				size_t at = data.size();
				data.insert(data.end(), raw.begin(), raw.end());
				return at;
			}

			size_t AddZeros(size_t count)
			{
				size_t at = data.size();
				data.resize(at + count, 0);
				return at;
			}

			void Align()
			{
				while (data.size() % 4 != 0)
				{
					data.push_back(0);
				}
			}

			void String(size_t owner, size_t field, const std::string& text)
			{
				strings.push_back({owner, field, text});
			}

			void WriteStrings()
			{
				std::map<std::string, size_t> table;

				for (const Ref& ref : strings)
				{
					auto found = table.find(ref.text);
					size_t at = 0;

					if (found == table.end())
					{
						at = data.size();
						data.insert(data.end(), ref.text.begin(), ref.text.end());
						data.push_back(0);
						table[ref.text] = at;
					}
					else
					{
						at = found->second;
					}

					PutI32(data, ref.field, static_cast<int32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(ref.owner)));
				}
			}
		};

		int32_t Checksum(const std::string& name)
		{
			unsigned char digest[32]{};
			std::string copy = name;
			BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(copy.data()), static_cast<ULONG>(copy.size()), digest, sizeof(digest));
			int32_t value = 0;
			std::memcpy(&value, digest, 4);
			return value;
		}

		// The bake map for this body: the given skin bones whose ancestor the body has, plus each part's own bones (ones the
		// body lacks) folded into the nearest body bone up the part's chain (or that bone's live ancestor).
		std::map<std::string, std::string> BakeMap(const MergeInput& input, const Body& body, const std::vector<Part>& parts)
		{
			std::map<std::string, std::string> out;

			for (const auto& entry : input.bake)
			{
				std::string name = Lower(entry.first);

				if (body.bones.count(name) != 0 && body.bones.count(Lower(entry.second)) != 0)
				{
					out[name] = entry.second;
				}
			}

			std::map<std::string, std::string> skeleton = out;

			for (const Part& part : parts)
			{
				for (const Bone& bone : part.bones)
				{
					std::string name = Lower(bone.name);

					if (body.bones.count(name) != 0)
					{
						continue;
					}

					int parent = bone.parent;
					std::string target;

					while (parent >= 0)
					{
						std::string parentName = Lower(part.bones[static_cast<size_t>(parent)].name);

						if (body.bones.count(parentName) != 0)
						{
							auto found = skeleton.find(parentName);
							target = found != skeleton.end() ? found->second : part.bones[static_cast<size_t>(parent)].name;
							break;
						}

						parent = part.bones[static_cast<size_t>(parent)].parent;
					}

					if (!target.empty())
					{
						out[name] = target;
					}
				}
			}

			return out;
		}

		void PlacePart(Part& part, const Body& body)
		{
			const Placement& place = part.input->placement;
			M34 local = AngleMatrix(place.ang[0], place.ang[1], place.ang[2], place.pos);

			if (place.scale != 1.0)
			{
				for (auto& row : local)
				{
					for (int j = 0; j < 3; ++j)
					{
						row[j] *= place.scale;
					}
				}
			}

			const Body::BodyBone* bone = nullptr;

			if (!place.attachment.empty())
			{
				auto attachment = body.attachments.find(Lower(place.attachment));

				if (attachment == body.attachments.end())
				{
					throw Failure("attachment " + place.attachment + " not on the body");
				}

				bone = &body.bones.at(Lower(attachment->second.bone));
				local = Mul(attachment->second.local, local);
			}
			else if (!place.bone.empty())
			{
				auto found = body.bones.find(Lower(place.bone));

				if (found == body.bones.end())
				{
					throw Failure("bone " + place.bone + " not on the body");
				}

				bone = &found->second;
			}
			else
			{
				throw Failure("parented to the root, not to a bone or attachment");
			}

			part.positioned = true;
			part.rigidBoneName = bone->name;
			part.rigidBoneRaw = bone->raw;
			part.rigidMatrix = Mul(Inverse(PoseToBone(bone->raw)), local);
		}

		// The prototype's bake_vertex: a vertex blended by its bones' transforms (any bone missing from `transforms` stays),
		// the normal renormalized; only when one of its bones has a transform.
		void BlendVertex(Bytes& vert, const std::map<int, M34>& transforms)
		{
			int count = vert[15];
			bool any = false;

			for (int k = 0; k < count; ++k)
			{
				any = any || transforms.count(vert[12 + k]) != 0;
			}

			if (!any)
			{
				return;
			}

			float weights[3]{};
			float pos[3]{};
			float normal[3]{};
			std::memcpy(weights, vert.data(), 12);
			std::memcpy(pos, vert.data() + 16, 12);
			std::memcpy(normal, vert.data() + 28, 12);
			double p0[3] = {pos[0], pos[1], pos[2]};
			double n0[3] = {normal[0], normal[1], normal[2]};
			double outP[3]{};
			double outN[3]{};
			double total = 0.0;

			for (int k = 0; k < count; ++k)
			{
				double w = weights[k];
				total += w;
				auto found = transforms.find(vert[12 + k]);
				double p[3] = {p0[0], p0[1], p0[2]};
				double n[3] = {n0[0], n0[1], n0[2]};

				if (found != transforms.end())
				{
					double r[3]{};
					Rotate(found->second, p0, r);

					for (int i = 0; i < 3; ++i)
					{
						p[i] = r[i] + found->second[i][3];
					}

					Rotate(found->second, n0, n);
				}

				for (int i = 0; i < 3; ++i)
				{
					outP[i] += w * p[i];
					outN[i] += w * n[i];
				}
			}

			if (total <= 0)
			{
				return;
			}

			double length = std::sqrt(outN[0] * outN[0] + outN[1] * outN[1] + outN[2] * outN[2]);

			if (length == 0.0)
			{
				length = 1.0;
			}

			for (int i = 0; i < 3; ++i)
			{
				PutF32(vert, 16 + 4 * static_cast<size_t>(i), outP[i] / total);
				PutF32(vert, 28 + 4 * static_cast<size_t>(i), outN[i] / length);
			}
		}

		void TransformVertex(Bytes& vert, const M34& m)
		{
			float pos[3]{};
			float normal[3]{};
			std::memcpy(pos, vert.data() + 16, 12);
			std::memcpy(normal, vert.data() + 28, 12);
			double p[3] = {pos[0], pos[1], pos[2]};
			double n[3] = {normal[0], normal[1], normal[2]};
			double rp[3]{};
			double rn[3]{};
			Rotate(m, p, rp);
			Rotate(m, n, rn);

			for (int i = 0; i < 3; ++i)
			{
				PutF32(vert, 16 + 4 * i, rp[i] + m[i][3]);
				PutF32(vert, 28 + 4 * i, rn[i]);
			}
		}

		struct StripGroup
		{
			uint8_t flags = 0;
			Bytes verts;
			Bytes indices;
			std::vector<std::pair<Bytes, std::vector<std::pair<int32_t, int32_t>>>> strips;
		};

		std::pair<uint8_t, std::vector<StripGroup>> VtxMesh(const View& x, int64_t offset)
		{
			int numsg = x.I32(offset);
			int64_t sgoffset = x.I32(offset + 4);
			uint8_t flags = x.U8(offset + 8);

			if (numsg < 0 || numsg > 64)
			{
				throw Failure("a strip group count out of range");
			}

			std::vector<StripGroup> groups;

			for (int g = 0; g < numsg; ++g)
			{
				int64_t sg = offset + sgoffset + static_cast<int64_t>(g) * 25;
				int nv = x.I32(sg);
				int64_t vo = x.I32(sg + 4);
				int ni = x.I32(sg + 8);
				int64_t io = x.I32(sg + 12);
				int ns = x.I32(sg + 16);
				int64_t so = x.I32(sg + 20);

				if (nv < 0 || ni < 0 || ns < 0 || nv > kMaxVertices || ni > kMaxVertices * 3 || ns > 4096)
				{
					throw Failure("a strip group out of range");
				}

				StripGroup group;
				group.flags = x.U8(sg + 24);
				group.verts = x.Slice(sg + vo, static_cast<int64_t>(nv) * 9);
				group.indices = x.Slice(sg + io, static_cast<int64_t>(ni) * 2);

				for (int s = 0; s < ns; ++s)
				{
					int64_t st = sg + so + static_cast<int64_t>(s) * 27;
					Bytes header = x.Slice(st, 27);
					int nbsc = x.I32(st + 19);
					int64_t bsco = x.I32(st + 23);

					if (nbsc < 0 || nbsc > 256)
					{
						throw Failure("a bone state change count out of range");
					}

					std::vector<std::pair<int32_t, int32_t>> changes;

					for (int c = 0; c < nbsc; ++c)
					{
						changes.emplace_back(x.I32(st + bsco + 8 * c), x.I32(st + bsco + 8 * c + 4));
					}

					group.strips.emplace_back(std::move(header), std::move(changes));
				}

				groups.push_back(std::move(group));
			}

			return {flags, std::move(groups)};
		}

		void MergeOrThrow(const MergeInput& input, MergeOutput& output)
		{
			if (input.parts.empty() || input.parts.size() > kMaxParts)
			{
				throw Failure("no parts, or too many");
			}

			if (input.name.empty() || input.name.size() > 48)
			{
				throw Failure("a bad model name");
			}

			for (const PartInput& part : input.parts)
			{
				if (part.mdl.size() > kMaxFileBytes || part.vvd.size() > kMaxFileBytes || part.vtx.size() > kMaxFileBytes)
				{
					throw Failure("a part file too large");
				}
			}

			Body body(input.body);
			std::vector<Part> parts;
			parts.reserve(input.parts.size());

			for (const PartInput& partInput : input.parts)
			{
				parts.emplace_back(partInput);
			}

			for (Part& part : parts)
			{
				if (part.input->positioned)
				{
					PlacePart(part, body);
				}
			}

			std::map<std::string, std::string> ancestors = BakeMap(input, body, parts);

			// bones: one merged bone per (name, bind pose)
			std::vector<MergedBone> bones;
			std::map<std::string, std::vector<int>> boneIndex;
			std::vector<std::vector<int>> remaps;
			std::set<std::string> bakedNames;

			auto findOrAdd = [&](const std::string& name, const M34& bind, const Bytes& raw, int parent) -> int
			{
				std::string key = Lower(name);

				for (int index : boneIndex[key])
				{
					if (MaxDifference(PoseToBone(bones[static_cast<size_t>(index)].raw), bind) < 1e-3)
					{
						return index;
					}
				}

				MergedBone bone;
				bone.name = name;
				bone.raw = raw;

				for (int i = 0; i < 3; ++i)
				{
					for (int j = 0; j < 4; ++j)
					{
						PutF32(bone.raw, 96 + static_cast<size_t>(i * 4 + j) * 4, bind[i][j]);
					}
				}

				bone.parent = parent;
				bone.flags = 0x400;
				bones.push_back(std::move(bone));
				boneIndex[key].push_back(static_cast<int>(bones.size()) - 1);
				return static_cast<int>(bones.size()) - 1;
			};

			for (Part& part : parts)
			{
				if (part.positioned)
				{
					int target = findOrAdd(part.rigidBoneName, PoseToBone(part.rigidBoneRaw), part.rigidBoneRaw, -1);
					remaps.emplace_back(std::max<size_t>(part.bones.size(), 1), target);
					continue;
				}

				std::vector<int> remap;

				for (size_t index = 0; index < part.bones.size(); ++index)
				{
					const Bone& bone = part.bones[index];
					std::string key = Lower(bone.name);
					M34 ownBind = PoseToBone(bone.raw);
					auto ancestor = ancestors.find(key);

					if (ancestor != ancestors.end())
					{
						std::string ancestorKey = Lower(ancestor->second);

						if (body.bones.count(ancestorKey) != 0 && body.rest.count(ancestorKey) != 0)
						{
							int q = static_cast<int>(index);

							while (q >= 0 && body.rest.count(Lower(part.bones[static_cast<size_t>(q)].name)) == 0)
							{
								q = part.bones[static_cast<size_t>(q)].parent;
							}

							if (q >= 0)
							{
								M34 relative = Mul(Mul(Inverse(body.rest.at(ancestorKey)), body.rest.at(Lower(part.bones[static_cast<size_t>(q)].name))),
									Mul(Inverse(part.rest[static_cast<size_t>(q)]), part.rest[index]));
								bakedNames.insert(bone.name);
								remap.push_back(findOrAdd(ancestor->second, Mul(relative, ownBind), body.bones.at(ancestorKey).raw, -1));
								continue;
							}
						}
					}

					int parent = bone.parent >= 0 ? remap[static_cast<size_t>(bone.parent)] : -1;
					int target = findOrAdd(bone.name, ownBind, bone.raw, parent);
					int sourceFlags = 0;
					std::memcpy(&sourceFlags, bone.raw.data() + 160, 4);
					bones[static_cast<size_t>(target)].flags |= sourceFlags;
					remap.push_back(target);
				}

				remaps.push_back(std::move(remap));
			}

			if (static_cast<int>(bones.size()) > kMaxBones)
			{
				throw Failure("more than 255 bones");
			}

			int rebinds = 0;

			for (const auto& entry : boneIndex)
			{
				rebinds += static_cast<int>(entry.second.size()) - (entry.second.empty() ? 0 : 1);
			}

			// meshes and textures
			std::vector<std::string> textures;
			std::map<std::string, int> textureIndex;
			std::vector<Mesh> meshes;

			for (size_t p = 0; p < parts.size(); ++p)
			{
				for (Mesh& mesh : parts[p].ChosenMeshes())
				{
					std::string path = parts[p].MaterialPath(mesh.texture);
					std::string key = Lower(path);

					if (textureIndex.count(key) == 0)
					{
						textureIndex[key] = static_cast<int>(textures.size());
						textures.push_back(path);
					}

					mesh.material = textureIndex[key];
					mesh.part = static_cast<int>(p);
					meshes.push_back(std::move(mesh));
				}
			}

			int32_t checksum = Checksum(input.name);

			// VVD
			std::vector<Bytes> outVerts;
			std::vector<Bytes> outTans;

			for (Mesh& mesh : meshes)
			{
				const std::vector<int>& remap = remaps[static_cast<size_t>(mesh.part)];
				const Part& part = parts[static_cast<size_t>(mesh.part)];
				mesh.offset = static_cast<int>(outVerts.size());
				std::map<int, M34> poseT;
				std::vector<int> firstBones;

				if (part.positioned)
				{
					for (size_t i = 0; i < part.bones.size(); ++i)
					{
						poseT[static_cast<int>(i)] = Mul(part.pose[i], PoseToBone(part.bones[i].raw));
					}
				}

				for (const Bytes& raw : mesh.verts)
				{
					Bytes vert = raw;
					int count = vert[15];
					firstBones.push_back(vert[12]);

					if (part.positioned)
					{
						BlendVertex(vert, poseT);
					}

					for (int k = 0; k < 3; ++k)
					{
						if (k < count)
						{
							int source = vert[12 + k];

							if (source >= static_cast<int>(remap.size()))
							{
								throw Failure("a vertex weighted to a missing bone");
							}

							vert[12 + k] = static_cast<uint8_t>(remap[static_cast<size_t>(source)]);
						}
						else
						{
							vert[12 + k] = 0;
						}
					}

					if (part.positioned)
					{
						TransformVertex(vert, part.rigidMatrix);
					}

					outVerts.push_back(std::move(vert));
				}

				for (size_t k = 0; k < mesh.tans.size(); ++k)
				{
					const Bytes& raw = mesh.tans[k];

					if (!part.positioned)
					{
						outTans.push_back(raw);
						continue;
					}

					Bytes tan = raw;
					float t[3]{};
					std::memcpy(t, tan.data(), 12);
					double v[3] = {t[0], t[1], t[2]};

					if (k < firstBones.size())
					{
						auto own = poseT.find(firstBones[k]);

						if (own != poseT.end())
						{
							double rotated[3]{};
							Rotate(own->second, v, rotated);
							v[0] = rotated[0];
							v[1] = rotated[1];
							v[2] = rotated[2];
						}
					}

					double r[3]{};
					Rotate(part.rigidMatrix, v, r);

					for (int i = 0; i < 3; ++i)
					{
						PutF32(tan, static_cast<size_t>(4 * i), r[i]);
					}

					outTans.push_back(std::move(tan));
				}
			}

			int total = static_cast<int>(outVerts.size());

			if (total > kMaxVertices)
			{
				throw Failure("too many vertices");
			}

			Bytes& vvd = output.vvd;
			vvd.clear();
			vvd.insert(vvd.end(), {'I', 'D', 'S', 'V'});
			AppendI32(vvd, 4);
			AppendI32(vvd, checksum);
			AppendI32(vvd, 1);

			for (int k = 0; k < 8; ++k)
			{
				AppendI32(vvd, total);
			}

			AppendI32(vvd, 0);
			AppendI32(vvd, 64);
			AppendI32(vvd, 64);
			AppendI32(vvd, 64 + total * kVertexSize);

			for (const Bytes& vert : outVerts)
			{
				vvd.insert(vvd.end(), vert.begin(), vert.end());
			}

			for (const Bytes& tan : outTans)
			{
				vvd.insert(vvd.end(), tan.begin(), tan.end());
			}

			// VTX: one bodypart, one model, one LOD, the meshes' strip groups whole with their bone state changes remapped
			Bytes& vtx = output.vtx;
			vtx.assign(36, 0);
			PutI32(vtx, 0, 7);
			PutI32(vtx, 4, 24);
			PutI16(vtx, 8, 53);
			PutI16(vtx, 10, 9);
			PutI32(vtx, 12, 3);
			PutI32(vtx, 16, checksum);
			PutI32(vtx, 20, 1);
			PutI32(vtx, 24, 0);
			PutI32(vtx, 28, 1);
			PutI32(vtx, 32, 36);
			AppendI32(vtx, 1);
			AppendI32(vtx, 8);
			AppendI32(vtx, 1);
			AppendI32(vtx, 8);
			AppendI32(vtx, static_cast<int32_t>(meshes.size()));
			AppendI32(vtx, 12);
			AppendF32(vtx, 0.0);
			size_t meshHeaders = vtx.size();
			vtx.resize(vtx.size() + 9 * meshes.size(), 0);

			std::vector<std::pair<uint8_t, std::vector<StripGroup>>> parsed;

			for (const Mesh& mesh : meshes)
			{
				parsed.push_back(VtxMesh(parts[static_cast<size_t>(mesh.part)].x, mesh.vtx));
			}

			std::vector<size_t> groupHeaders;

			for (size_t k = 0; k < parsed.size(); ++k)
			{
				size_t at = vtx.size();
				size_t header = meshHeaders + 9 * k;
				PutI32(vtx, header, static_cast<int32_t>(parsed[k].second.size()));
				PutI32(vtx, header + 4, static_cast<int32_t>(at - header));
				vtx[header + 8] = parsed[k].first;
				vtx.resize(vtx.size() + 25 * parsed[k].second.size(), 0);
				groupHeaders.push_back(at);
			}

			for (size_t k = 0; k < parsed.size(); ++k)
			{
				const std::vector<int>& remap = remaps[static_cast<size_t>(meshes[k].part)];

				for (size_t g = 0; g < parsed[k].second.size(); ++g)
				{
					const StripGroup& group = parsed[k].second[g];
					size_t sg = groupHeaders[k] + 25 * g;
					size_t vo = vtx.size() - sg;
					vtx.insert(vtx.end(), group.verts.begin(), group.verts.end());
					size_t io = vtx.size() - sg;
					vtx.insert(vtx.end(), group.indices.begin(), group.indices.end());
					size_t so = vtx.size() - sg;
					size_t stripAt = vtx.size();
					vtx.resize(vtx.size() + 27 * group.strips.size(), 0);

					for (size_t s = 0; s < group.strips.size(); ++s)
					{
						size_t st = stripAt + 27 * s;
						size_t bsco = vtx.size() - st;

						for (const auto& change : group.strips[s].second)
						{
							if (change.second < 0 || change.second >= static_cast<int>(remap.size()))
							{
								throw Failure("a bone state change on a missing bone");
							}

							AppendI32(vtx, change.first);
							AppendI32(vtx, remap[static_cast<size_t>(change.second)]);
						}

						Bytes header = group.strips[s].first;
						PutI32(header, 19, static_cast<int32_t>(group.strips[s].second.size()));
						PutI32(header, 23, static_cast<int32_t>(bsco));
						std::memcpy(vtx.data() + st, header.data(), 27);
					}

					PutI32(vtx, sg, static_cast<int32_t>(group.verts.size() / 9));
					PutI32(vtx, sg + 4, static_cast<int32_t>(vo));
					PutI32(vtx, sg + 8, static_cast<int32_t>(group.indices.size() / 2));
					PutI32(vtx, sg + 12, static_cast<int32_t>(io));
					PutI32(vtx, sg + 16, static_cast<int32_t>(group.strips.size()));
					PutI32(vtx, sg + 20, static_cast<int32_t>(so));
					vtx[sg + 24] = group.flags;
				}
			}

			size_t matrep = vtx.size();
			AppendI32(vtx, 0);
			AppendI32(vtx, 0);
			PutI32(vtx, 24, static_cast<int32_t>(matrep));

			// MDL
			Writer w;
			w.AddZeros(kHeaderSize);
			size_t h2 = w.AddZeros(kHeader2Size);
			const Part& first = parts[0];
			std::memcpy(w.data.data(), "IDST", 4);
			PutI32(w.data, 4, 48);
			PutI32(w.data, 8, checksum);
			std::string modelName = "rcx_merged/" + input.name + ".mdl";
			std::memcpy(w.data.data() + 12, modelName.data(), std::min<size_t>(modelName.size(), 63));

			double vectors[18]{};
			std::copy(std::begin(first.vectors), std::end(first.vectors), vectors);

			for (size_t p = 1; p < parts.size(); ++p)
			{
				for (int k = 0; k < 3; ++k)
				{
					vectors[6 + k] = std::min(vectors[6 + k], parts[p].vectors[6 + k]);
					vectors[9 + k] = std::max(vectors[9 + k], parts[p].vectors[9 + k]);
					vectors[12 + k] = std::min(vectors[12 + k], parts[p].vectors[12 + k]);
					vectors[15 + k] = std::max(vectors[15 + k], parts[p].vectors[15 + k]);
				}
			}

			for (int k = 0; k < 18; ++k)
			{
				PutF32(w.data, 80 + 4 * static_cast<size_t>(k), vectors[k]);
			}

			PutI32(w.data, 152, first.flags);

			enum Field
			{
				numbones, boneindex, numbonecontrollers, bonecontrollerindex, numhitboxsets, hitboxsetindex, numlocalanim, localanimindex,
				numlocalseq, localseqindex, activitylistversion, eventsindexed, numtextures, textureindex, numcdtextures, cdtextureindex,
				numskinref, numskinfamilies, skinindex, numbodyparts, bodypartindex, numlocalattachments, localattachmentindex,
				numlocalnodes, localnodeindex, localnodenameindex, numflexdesc, flexdescindex, numflexcontrollers, flexcontrollerindex,
				numflexrules, flexruleindex, numikchains, ikchainindex, nummouths, mouthindex, numlocalposeparameters, localposeparamindex,
				surfacepropindex, keyvalueindex, keyvaluesize, numlocalikautoplaylocks, localikautoplaylockindex, mass, contents,
				numincludemodels, includemodelindex, virtualModel, szanimblocknameindex, numanimblocks, animblockindex, animblockModel,
				bonetablebynameindex, kFieldCount
			};
			std::array<int32_t, kFieldCount> header{};

			w.Align();
			header[boneindex] = static_cast<int32_t>(w.data.size());

			for (const MergedBone& bone : bones)
			{
				size_t at = w.Add(bone.raw);
				w.String(at, at, bone.name);
				PutI32(w.data, at + 4, bone.parent);
				PutI32(w.data, at + 160, bone.flags);

				for (int c = 0; c < 6; ++c)
				{
					PutI32(w.data, at + 8 + 4 * static_cast<size_t>(c), -1);
				}

				PutI32(w.data, at + 164, 0);
				PutI32(w.data, at + 168, 0);
				w.String(at, at + 176, "");
			}

			header[hitboxsetindex] = static_cast<int32_t>(w.data.size());
			size_t hitbox = w.AddZeros(12);
			PutI32(w.data, hitbox + 8, 12);
			w.String(hitbox, hitbox, "default");

			header[bonetablebynameindex] = static_cast<int32_t>(w.data.size());
			std::vector<int> order(bones.size());

			for (size_t k = 0; k < order.size(); ++k)
			{
				order[k] = static_cast<int>(k);
			}

			std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return Lower(bones[static_cast<size_t>(a)].name) < Lower(bones[static_cast<size_t>(b)].name); });

			for (int index : order)
			{
				w.data.push_back(static_cast<uint8_t>(index));
			}

			w.Align();

			// one animation with no bone data: every bone takes its bind pose, which bonemerge overrides
			size_t animAt = w.AddZeros(100);
			header[localanimindex] = static_cast<int32_t>(animAt);
			PutI32(w.data, animAt, -static_cast<int32_t>(animAt));
			w.String(animAt, animAt + 4, "@idle");
			PutF32(w.data, animAt + 8, 30.0);
			PutI32(w.data, animAt + 12, 0);
			PutI32(w.data, animAt + 16, 1);
			size_t empty = w.Add({255, 0, 0, 0});
			PutI32(w.data, animAt + 56, static_cast<int32_t>(empty - animAt));
			w.Align();

			size_t seqAt = w.AddZeros(212);
			header[localseqindex] = static_cast<int32_t>(seqAt);
			PutI32(w.data, seqAt, -static_cast<int32_t>(seqAt));
			w.String(seqAt, seqAt + 4, "idle");
			w.String(seqAt, seqAt + 8, "");
			PutI32(w.data, seqAt + 12, 0);
			PutI32(w.data, seqAt + 16, -1);
			PutI32(w.data, seqAt + 20, 0);

			for (int k = 0; k < 6; ++k)
			{
				PutF32(w.data, seqAt + 32 + 4 * static_cast<size_t>(k), vectors[6 + k]);
			}

			PutI32(w.data, seqAt + 56, 1);
			size_t animIndex = w.AddZeros(4);
			PutI32(w.data, seqAt + 60, static_cast<int32_t>(animIndex - seqAt));
			PutI32(w.data, seqAt + 68, 1);
			PutI32(w.data, seqAt + 72, 1);
			PutI32(w.data, seqAt + 76, -1);
			PutI32(w.data, seqAt + 80, -1);
			PutI32(w.data, seqAt + 100, 0);
			PutF32(w.data, seqAt + 104, 0.2);
			PutF32(w.data, seqAt + 108, 0.2);
			size_t weights = w.data.size();

			for (size_t k = 0; k < bones.size(); ++k)
			{
				AppendF32(w.data, 1.0);
			}

			PutI32(w.data, seqAt + 156, static_cast<int32_t>(weights - seqAt));
			PutI32(w.data, seqAt + 180, 0);

			w.Align();
			header[bodypartindex] = static_cast<int32_t>(w.data.size());
			size_t bpAt = w.AddZeros(16);
			PutI32(w.data, bpAt + 4, 1);
			PutI32(w.data, bpAt + 8, 1);
			PutI32(w.data, bpAt + 12, 16);
			w.String(bpAt, bpAt, "merged");
			size_t modelAt = w.AddZeros(kModelSize);
			std::memcpy(w.data.data() + modelAt, "merged", 6);
			double radius = 0.0;

			for (int k = 6; k < 12; ++k)
			{
				radius = std::max(radius, std::abs(vectors[k]));
			}

			PutI32(w.data, modelAt + 64, 0);
			PutF32(w.data, modelAt + 68, radius * 1.8);
			size_t meshesAt = w.data.size();
			PutI32(w.data, modelAt + 72, static_cast<int32_t>(meshes.size()));
			PutI32(w.data, modelAt + 76, static_cast<int32_t>(meshesAt - modelAt));
			PutI32(w.data, modelAt + 80, total);
			PutI32(w.data, modelAt + 84, 0);
			PutI32(w.data, modelAt + 88, 0);

			for (size_t k = 0; k < meshes.size(); ++k)
			{
				const Mesh& mesh = meshes[k];
				size_t at = w.Add(mesh.raw);
				int n = static_cast<int>(mesh.verts.size());
				PutI32(w.data, at, mesh.material);
				PutI32(w.data, at + 4, static_cast<int32_t>(static_cast<int64_t>(modelAt) - static_cast<int64_t>(at)));
				PutI32(w.data, at + 8, n);
				PutI32(w.data, at + 12, mesh.offset);
				PutI32(w.data, at + 16, 0);
				PutI32(w.data, at + 20, 0);
				PutI32(w.data, at + 32, static_cast<int32_t>(k));
				PutI32(w.data, at + 48, 0);

				for (int lod = 0; lod < 8; ++lod)
				{
					PutI32(w.data, at + 52 + 4 * static_cast<size_t>(lod), n);
				}
			}

			w.Align();
			header[textureindex] = static_cast<int32_t>(w.data.size());

			for (const std::string& path : textures)
			{
				size_t at = w.AddZeros(kTextureSize);
				w.String(at, at, path);
			}

			header[cdtextureindex] = static_cast<int32_t>(w.data.size());
			size_t cdAt = w.AddZeros(4);
			header[skinindex] = static_cast<int32_t>(w.data.size());

			for (size_t k = 0; k < textures.size(); ++k)
			{
				size_t at = w.AddZeros(2);
				PutI16(w.data, at, static_cast<int16_t>(k));
			}

			w.Align();

			int32_t blank = static_cast<int32_t>(w.data.size());

			for (Field field : {keyvalueindex, bonecontrollerindex, localattachmentindex, localnodeindex, localnodenameindex, flexdescindex,
				flexcontrollerindex, flexruleindex, ikchainindex, mouthindex, localposeparamindex, localikautoplaylockindex, includemodelindex,
				animblockindex})
			{
				header[field] = blank;
			}

			header[numbones] = static_cast<int32_t>(bones.size());
			header[numhitboxsets] = 1;
			header[numlocalanim] = 1;
			header[numlocalseq] = 1;
			header[numtextures] = static_cast<int32_t>(textures.size());
			header[numcdtextures] = 1;
			header[numskinref] = static_cast<int32_t>(textures.size());
			header[numskinfamilies] = 1;
			header[numbodyparts] = 1;
			header[contents] = 1;

			for (int k = 0; k < kFieldCount; ++k)
			{
				if (k == mass)
				{
					PutF32(w.data, 156 + 4 * static_cast<size_t>(k), 1.0);
				}
				else
				{
					PutI32(w.data, 156 + 4 * static_cast<size_t>(k), header[static_cast<size_t>(k)]);
				}
			}

			// studiohdr2index; the fields between bonetablebynameindex and it stay 0, as in the prototype
			PutI32(w.data, 400, static_cast<int32_t>(h2));

			std::string firstSurface;

			if (!first.bones.empty())
			{
				View fm{first.input->mdl, "part mdl"};
				int64_t o = first.boneindex;
				int surface = fm.I32(o + 176);
				firstSurface = surface != 0 ? fm.CStr(o + surface) : std::string();
			}

			w.String(0, 156 + 4 * static_cast<size_t>(surfacepropindex), firstSurface);
			w.String(0, 156 + 4 * static_cast<size_t>(szanimblocknameindex), "");
			w.String(h2, h2 + 20, modelName);
			w.String(0, cdAt, "");
			w.WriteStrings();
			w.Align();
			PutI32(w.data, 76, static_cast<int32_t>(w.data.size()));
			PutF32(w.data, h2 + 12, 0.0);

			output.mdl = std::move(w.data);
			output.bones = static_cast<int>(bones.size());
			output.baked = static_cast<int>(bakedNames.size());
			output.rebinds = rebinds;
			output.meshes = static_cast<int>(meshes.size());
			output.vertices = total;
		}
	}

	bool Merge(const MergeInput& input, MergeOutput& output, std::string& error)
	{
		try
		{
			MergeOrThrow(input, output);
			return true;
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
			return false;
		}
	}
}
