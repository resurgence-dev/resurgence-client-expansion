/*
	The golden test of the merged-mesh builder (src/mesh/mesh_merge.cpp): for every case folder under the given fixture
	folder (written by tools/mesh_prototype/merge_cases.py --bake --fixtures <dir>), merges the case's inputs and checks
	the three files are byte for byte the Python prototype's. Then checks a few refusals of hostile input. Exits non-zero
	on any failure.
*/
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mesh/mesh_merge.h"

namespace
{
	int failures = 0;

	rcx::mesh::Bytes ReadFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return rcx::mesh::Bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	std::vector<std::string> Split(const std::string& line)
	{
		std::vector<std::string> fields;
		std::stringstream stream(line);
		std::string field;

		while (std::getline(stream, field, '\t'))
		{
			fields.push_back(field);
		}

		if (!line.empty() && line.back() == '\t')
		{
			fields.push_back("");
		}

		return fields;
	}

	void Compare(const std::string& label, const char* what, const rcx::mesh::Bytes& got, const rcx::mesh::Bytes& expected)
	{
		if (got == expected)
		{
			return;
		}

		size_t at = 0;

		while (at < got.size() && at < expected.size() && got[at] == expected[at])
		{
			++at;
		}

		std::printf("FAIL %s: %s differs from the prototype at byte %zu (%zu bytes against %zu)\n", label.c_str(), what, at, got.size(), expected.size());
		failures += 1;
	}

	bool LoadCase(const std::filesystem::path& folder, rcx::mesh::MergeInput& input)
	{
		std::ifstream file(folder / "case.txt");

		if (!file)
		{
			return false;
		}

		input.body = ReadFile(folder / "body.mdl");
		std::string line;

		while (std::getline(file, line))
		{
			std::vector<std::string> f = Split(line);

			if (f.empty())
			{
				continue;
			}

			if (f[0] == "name")
			{
				input.name = f[1];
			}
			else if (f[0] == "bake")
			{
				input.bake.emplace_back(f[1], f[2]);
			}
			else if (f[0] == "part")
			{
				size_t index = std::stoul(f[1]);
				input.parts.resize(index + 1);
				rcx::mesh::PartInput& part = input.parts[index];
				std::string stem = "part" + f[1];
				part.mdl = ReadFile(folder / (stem + ".mdl"));
				part.vvd = ReadFile(folder / (stem + ".vvd"));
				part.vtx = ReadFile(folder / (stem + ".vtx"));
				part.skin = std::stoi(f[2]);
				part.material = f[3];
				part.positioned = f[4] == "1";
			}
			else if (f[0] == "bodygroup")
			{
				input.parts[std::stoul(f[1])].bodygroups.emplace_back(f[2], std::stoi(f[3]));
			}
			else if (f[0] == "material")
			{
				input.parts[std::stoul(f[1])].materials.push_back(f[2]);
			}
			else if (f[0] == "place")
			{
				rcx::mesh::Placement& place = input.parts[std::stoul(f[1])].placement;
				place.bone = f[2];
				place.attachment = f[3];

				for (int k = 0; k < 3; ++k)
				{
					place.pos[k] = std::stod(f[4 + static_cast<size_t>(k)]);
					place.ang[k] = std::stod(f[7 + static_cast<size_t>(k)]);
				}

				place.scale = std::stod(f[10]);
			}
		}

		return true;
	}

	void Refuses(const char* what, rcx::mesh::MergeInput input)
	{
		rcx::mesh::MergeOutput output;
		std::string error;
		bool merged = rcx::mesh::Merge(input, output, error);
		std::printf("%s refuses %s%s%s\n", merged ? "FAIL" : "ok  ", what, merged ? "" : ": ", error.c_str());

		if (merged)
		{
			failures += 1;
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: rcx_mesh_merge_test <fixture folder>\n");
		return 2;
	}

	std::filesystem::path root = argv[1];
	int cases = 0;
	rcx::mesh::MergeInput sample;

	for (const auto& entry : std::filesystem::directory_iterator(root))
	{
		rcx::mesh::MergeInput input;

		if (!entry.is_directory() || !LoadCase(entry.path(), input))
		{
			continue;
		}

		std::string label = entry.path().filename().string();
		rcx::mesh::MergeOutput output;
		std::string error;
		cases += 1;

		if (!rcx::mesh::Merge(input, output, error))
		{
			std::printf("FAIL %s: refused: %s\n", label.c_str(), error.c_str());
			failures += 1;
			continue;
		}

		Compare(label, "mdl", output.mdl, ReadFile(entry.path() / "expect.mdl"));
		Compare(label, "vvd", output.vvd, ReadFile(entry.path() / "expect.vvd"));
		Compare(label, "vtx", output.vtx, ReadFile(entry.path() / "expect.vtx"));
		std::printf("ok   %s: %d bones (%d baked, %d same-named with another bind), %d meshes, %d vertices\n", label.c_str(), output.bones,
			output.baked, output.rebinds, output.meshes, output.vertices);

		if (sample.parts.empty())
		{
			sample = input;
		}
	}

	if (cases == 0)
	{
		std::printf("FAIL no fixture cases under %s\n", argv[1]);
		return 1;
	}

	rcx::mesh::MergeInput truncated = sample;
	truncated.parts[0].mdl.resize(truncated.parts[0].mdl.size() / 2);
	Refuses("a part MDL cut in half", truncated);

	rcx::mesh::MergeInput wrongVersion = sample;
	wrongVersion.parts[0].vtx[0] = 6;
	Refuses("a VTX of the wrong version", wrongVersion);

	rcx::mesh::MergeInput badOffset = sample;
	int32_t far = 0x7ffffff0;
	std::memcpy(badOffset.parts[0].mdl.data() + 160, &far, 4);
	Refuses("a bone table past the end of the file", badOffset);

	rcx::mesh::MergeInput noParts = sample;
	noParts.parts.clear();
	Refuses("no parts", noParts);

	rcx::mesh::MergeInput longName = sample;
	longName.name = std::string(64, 'a');
	Refuses("a model name over 48 characters", longName);

	std::printf("%d case(s), %d failure(s)\n", cases, failures);
	return failures == 0 ? 0 : 1;
}
