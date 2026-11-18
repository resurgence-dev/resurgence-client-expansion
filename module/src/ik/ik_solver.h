#pragma once

/*
	The body IK solver: numbers in, numbers out. No engine access, no Lua, no allocation, so it
	builds and runs anywhere and is unit tested outside the game (tests/ik_solver_test.cpp).
*/
namespace rcx::ik
{
	struct Vec3
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	struct ArmInput
	{
		Vec3 shoulder{};
		Vec3 target{};

		// A point the elbow bends toward (any point off the shoulder to target line).
		Vec3 pole{};
		double upperLength = 0.0;
		double lowerLength = 0.0;
	};

	struct ArmResult
	{
		Vec3 elbow{};

		// The target, pulled in to the arm's reach when it was out of it.
		Vec3 hand{};
		bool reached = false;
	};

	// The largest coordinate and length the solver accepts; Lua's arguments are checked against them.
	constexpr double kMaxCoordinate = 1.0e6;
	constexpr double kMaxLength = 1.0e4;

	// Whether the input is finite, in range and the lengths positive.
	bool IsValid(const ArmInput& input);

	// Two-bone solve: the elbow on the side of the pole, the upper arm and forearm keeping their
	// lengths, the hand at the target or as near it as the arm reaches. Returns false for invalid input.
	bool SolveArm(const ArmInput& input, ArmResult& result);
}
