/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Settings to aid debugging.
 */

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace solidity::frontend
{

enum class RevertStrings
{

	Default, // no compiler-generated strings, keep user-supplied strings
	Strip, // no compiler-generated strings, remove user-supplied strings (if possible)
	Debug, // add strings for internal reverts, keep user-supplied strings
	VerboseDebug // add strings for internal reverts, add user-supplied strings if not provided
};

inline constexpr std::array<std::pair<RevertStrings, std::string_view>, 4> c_revertStringsTable{{
	{RevertStrings::Default, "default"},
	{RevertStrings::Strip, "strip"},
	{RevertStrings::Debug, "debug"},
	{RevertStrings::VerboseDebug, "verboseDebug"},
}};

inline constexpr std::array<std::string_view, c_revertStringsTable.size()> allRevertStringsNames() noexcept
{
	std::array<std::string_view, c_revertStringsTable.size()> names{};
	for (size_t i = 0; i < c_revertStringsTable.size(); ++i)
		names[i] = c_revertStringsTable[i].second;
	return names;
}

inline std::string revertStringsToString(RevertStrings _str)
{
	for (auto const& [value, name]: c_revertStringsTable)
		if (value == _str)
			return std::string(name);
	// Cannot reach this.
	return "INVALID";
}

inline std::optional<RevertStrings> revertStringsFromString(std::string_view _str)
{
	for (auto const& [value, name]: c_revertStringsTable)
		if (name == _str)
			return value;
	return std::nullopt;
}

}
