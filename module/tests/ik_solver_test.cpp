/*
	Unit tests for the body IK solver (src/ik/ik_solver.cpp), run outside the game:
	builds/win64/rcx_ik_test.exe, built and run by tools/build_module.ps1. Prints one line per check
	and exits non-zero when any fails.
*/
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#include "ik/ik_solver.h"

using rcx::ik::ArmInput;
using rcx::ik::ArmResult;
using rcx::ik::Vec3;

namespace
{
	int failures = 0;
	int checks = 0;

	void Check(bool passed, const char* what)
	{
		checks++;

		if (!passed)
		{
			failures++;
		}

		std::printf("%s %s\n", passed ? "ok  " : "FAIL", what);
	}

	double Distance(const Vec3& a, const Vec3& b)
	{
		double x = a.x - b.x;
		double y = a.y - b.y;
		double z = a.z - b.z;
		return std::sqrt(x * x + y * y + z * z);
	}

	double Dot(const Vec3& a, const Vec3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	Vec3 Subtract(const Vec3& a, const Vec3& b)
	{
		return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
	}

	// A right arm in Source units: shoulder at the origin, the hand forward and down, the elbow toward the pole below.
	ArmInput MakeArm()
	{
		ArmInput input{};
		input.shoulder = Vec3{0.0, 0.0, 0.0};
		input.target = Vec3{30.0, -5.0, -10.0};
		input.pole = Vec3{0.0, -10.0, -40.0};
		input.upperLength = 20.0;
		input.lowerLength = 18.0;
		return input;
	}

	void TestReachable()
	{
		ArmInput input = MakeArm();
		ArmResult result{};

		Check(rcx::ik::SolveArm(input, result), "reachable: solves");
		Check(result.reached, "reachable: reports reached");
		Check(Distance(result.hand, input.target) < 1.0e-6, "reachable: the hand is at the target");
		Check(std::fabs(Distance(result.elbow, input.shoulder) - input.upperLength) < 1.0e-6, "reachable: the upper arm keeps its length");
		Check(std::fabs(Distance(result.elbow, result.hand) - input.lowerLength) < 1.0e-6, "reachable: the forearm keeps its length");

		// The elbow lies on the pole's side of the shoulder to hand line.
		Vec3 axis = Subtract(input.target, input.shoulder);
		Vec3 toElbow = Subtract(result.elbow, input.shoulder);
		Vec3 toPole = Subtract(input.pole, input.shoulder);
		double axisLength2 = Dot(axis, axis);
		Vec3 elbowOff = Subtract(toElbow, Vec3{axis.x * Dot(toElbow, axis) / axisLength2, axis.y * Dot(toElbow, axis) / axisLength2, axis.z * Dot(toElbow, axis) / axisLength2});
		Vec3 poleOff = Subtract(toPole, Vec3{axis.x * Dot(toPole, axis) / axisLength2, axis.y * Dot(toPole, axis) / axisLength2, axis.z * Dot(toPole, axis) / axisLength2});
		Check(Dot(elbowOff, poleOff) > 0.0, "reachable: the elbow bends toward the pole");
	}

	void TestOutOfReach()
	{
		ArmInput input = MakeArm();
		input.target = Vec3{100.0, 0.0, 0.0};
		ArmResult result{};

		Check(rcx::ik::SolveArm(input, result), "out of reach: solves");
		Check(!result.reached, "out of reach: reports not reached");
		Check(Distance(result.hand, input.shoulder) < input.upperLength + input.lowerLength, "out of reach: the hand stops inside full reach");
		Check(Distance(result.hand, input.shoulder) > (input.upperLength + input.lowerLength) * 0.99, "out of reach: the arm is nearly straight");
		Check(result.hand.y == 0.0 && result.hand.z == 0.0 && result.hand.x > 0.0, "out of reach: the hand points at the target");
	}

	void TestTooClose()
	{
		ArmInput input = MakeArm();
		input.target = Vec3{0.5, 0.0, 0.0};
		input.lowerLength = 12.0;
		ArmResult result{};

		Check(rcx::ik::SolveArm(input, result), "too close: solves");
		Check(!result.reached, "too close: reports not reached");
		Check(std::fabs(Distance(result.elbow, result.hand) - input.lowerLength) < 1.0e-6, "too close: the forearm keeps its length");
	}

	void TestDegenerate()
	{
		ArmInput input = MakeArm();
		input.pole = Vec3{60.0, -10.0, -20.0};
		ArmResult result{};

		Check(rcx::ik::SolveArm(input, result), "pole on the arm's line: solves");
		Check(std::isfinite(result.elbow.x) && std::isfinite(result.elbow.y) && std::isfinite(result.elbow.z), "pole on the arm's line: the elbow is finite");
		Check(result.elbow.z < 0.0, "pole on the arm's line: the elbow falls back to bending down");

		input = MakeArm();
		input.target = input.shoulder;
		Check(rcx::ik::SolveArm(input, result), "target at the shoulder: solves");
		Check(std::isfinite(result.elbow.x) && std::isfinite(result.hand.x), "target at the shoulder: finite");
	}

	void TestInvalid()
	{
		ArmResult result{};
		ArmInput input = MakeArm();
		input.upperLength = 0.0;
		Check(!rcx::ik::SolveArm(input, result), "invalid: a zero length is refused");

		input = MakeArm();
		input.target.x = std::numeric_limits<double>::quiet_NaN();
		Check(!rcx::ik::SolveArm(input, result), "invalid: NaN is refused");

		input = MakeArm();
		input.pole.z = std::numeric_limits<double>::infinity();
		Check(!rcx::ik::SolveArm(input, result), "invalid: infinity is refused");

		input = MakeArm();
		input.shoulder.y = 2.0e6;
		Check(!rcx::ik::SolveArm(input, result), "invalid: a coordinate past 1e6 is refused");

		input = MakeArm();
		input.lowerLength = -3.0;
		Check(!rcx::ik::SolveArm(input, result), "invalid: a negative length is refused");
	}

	void Bench()
	{
		constexpr int kIterations = 1000000;
		ArmInput input = MakeArm();
		ArmResult result{};
		double sink = 0.0;
		auto start = std::chrono::steady_clock::now();

		for (int index = 0; index < kIterations; index++)
		{
			input.target.x = 20.0 + (index % 100) * 0.1;
			rcx::ik::SolveArm(input, result);
			sink += result.elbow.z;
		}

		std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
		std::printf("bench: %.1f ns per arm solve, without the Lua call (%g)\n", elapsed.count() / kIterations, sink > 0.0 ? 1.0 : 0.0);
	}
}

int main()
{
	TestReachable();
	TestOutOfReach();
	TestTooClose();
	TestDegenerate();
	TestInvalid();
	Bench();

	std::printf("%d of %d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
