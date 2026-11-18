#include "ik/ik_solver.h"

#include <algorithm>
#include <cmath>

namespace rcx::ik
{
	namespace
	{
		// How far inside full reach and full fold the hand is held, as a share of the arm, so the
		// elbow never locks straight or folds flat (where its direction is undefined).
		constexpr double kReachMargin = 0.001;
		constexpr double kTiny = 1.0e-9;

		Vec3 Add(const Vec3& a, const Vec3& b)
		{
			return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
		}

		Vec3 Subtract(const Vec3& a, const Vec3& b)
		{
			return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
		}

		Vec3 Scale(const Vec3& a, double scale)
		{
			return Vec3{a.x * scale, a.y * scale, a.z * scale};
		}

		double Dot(const Vec3& a, const Vec3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		double Length(const Vec3& a)
		{
			return std::sqrt(Dot(a, a));
		}

		bool IsFiniteInRange(double value, double limit)
		{
			return std::isfinite(value) && std::fabs(value) <= limit;
		}

		bool IsValidPoint(const Vec3& point)
		{
			return IsFiniteInRange(point.x, kMaxCoordinate) && IsFiniteInRange(point.y, kMaxCoordinate) && IsFiniteInRange(point.z, kMaxCoordinate);
		}

		bool IsValidLength(double length)
		{
			return std::isfinite(length) && length > kTiny && length <= kMaxLength;
		}

		// The part of `vector` square to the unit `axis`, normalized, or false when there is none.
		bool PerpendicularUnit(const Vec3& vector, const Vec3& axis, Vec3& result)
		{
			Vec3 perpendicular = Subtract(vector, Scale(axis, Dot(vector, axis)));
			double length = Length(perpendicular);

			if (length < kTiny)
			{
				return false;
			}

			result = Scale(perpendicular, 1.0 / length);
			return true;
		}

		// A bend direction square to `axis`: toward the pole, else down, else along x.
		Vec3 BendDirection(const Vec3& toPole, const Vec3& axis)
		{
			Vec3 bend{};

			if (PerpendicularUnit(toPole, axis, bend))
			{
				return bend;
			}

			if (PerpendicularUnit(Vec3{0.0, 0.0, -1.0}, axis, bend))
			{
				return bend;
			}

			PerpendicularUnit(Vec3{1.0, 0.0, 0.0}, axis, bend);
			return bend;
		}
	}

	bool IsValid(const ArmInput& input)
	{
		return IsValidPoint(input.shoulder) && IsValidPoint(input.target) && IsValidPoint(input.pole) && IsValidLength(input.upperLength) && IsValidLength(input.lowerLength);
	}

	bool SolveArm(const ArmInput& input, ArmResult& result)
	{
		if (!IsValid(input))
		{
			return false;
		}

		double upper = input.upperLength;
		double lower = input.lowerLength;
		double margin = (upper + lower) * kReachMargin;
		double shortest = std::fabs(upper - lower) + margin;
		double longest = upper + lower - margin;

		Vec3 toTarget = Subtract(input.target, input.shoulder);
		double distance = Length(toTarget);
		Vec3 axis{};

		if (distance > kTiny)
		{
			axis = Scale(toTarget, 1.0 / distance);
		}
		else
		{
			axis = Vec3{1.0, 0.0, 0.0};
		}

		double reach = std::clamp(distance, shortest, longest);
		result.reached = distance >= shortest && distance <= longest;
		result.hand = Add(input.shoulder, Scale(axis, reach));

		// Law of cosines at the shoulder: the elbow's distance along the axis and off it.
		double cosine = (upper * upper + reach * reach - lower * lower) / (2.0 * upper * reach);
		cosine = std::clamp(cosine, -1.0, 1.0);
		double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));

		Vec3 bend = BendDirection(Subtract(input.pole, input.shoulder), axis);
		result.elbow = Add(input.shoulder, Add(Scale(axis, upper * cosine), Scale(bend, upper * sine)));

		return true;
	}
}
