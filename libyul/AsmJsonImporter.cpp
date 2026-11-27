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
 * @author julius <djudju@protonmail.com>
 * @date 2019
 * Converts an inlineAssembly AST from JSON format to AsmData
 */

#include <libyul/AsmJsonImporter.h>

#include <libyul/AST.h>
#include <libyul/AsmJsonSchema.h>
#include <libyul/Dialect.h>
#include <libyul/Exceptions.h>
#include <libyul/Utilities.h>

#include <liblangutil/Exceptions.h>
#include <liblangutil/Scanner.h>

#include <rfl/nlohmann.hpp>

#include <fmt/format.h>

#include <vector>

using namespace solidity::langutil;

namespace solidity::yul
{

namespace
{

struct AstBuilder
{
	Dialect const& dialect;
	std::vector<std::shared_ptr<std::string const>> const& sourceNames;

	template <class T>
	T createAsmNode(std::string const& _src) const
	{
		T r;
		SourceLocation const nativeLocation = langutil::parseSourceLocation(_src, sourceNames);
		yulAssert(nativeLocation.hasText(), "Invalid source location in Asm AST");
		// TODO: We should add originLocation to the AST.
		// While it's not included, we'll use nativeLocation for it because we only support importing
		// inline assembly as a part of a Solidity AST and there these locations are always the same.
		r.debugData = DebugData::create(nativeLocation, nativeLocation);
		return r;
	}

	NameWithDebugData fromDto(ast_json::YulTypedName const& _dto) const
	{
		auto node = createAsmNode<NameWithDebugData>(_dto.src);
		node.name = YulName{_dto.name};
		return node;
	}

	Identifier fromDto(ast_json::YulIdentifier const& _dto) const
	{
		auto node = createAsmNode<Identifier>(_dto.src);
		node.name = YulName{_dto.name};
		return node;
	}

	Literal fromDto(ast_json::YulLiteral const& _dto) const
	{
		auto lit = createAsmNode<Literal>(_dto.src);
		solAssert(_dto.hexValue.has_value() || _dto.value.has_value(), "");
		std::string const value = _dto.hexValue
			? util::asString(util::fromHex(*_dto.hexValue))
			: *_dto.value;
		yulAssert(
			!_dto.type || _dto.type->empty(),
			fmt::format(
				"Expected literal types to be either empty or absent in the JSON. Got \"{}\".",
				_dto.type.value_or("")
			)
		);

		std::string const kind = _dto.kind.str();
		if (kind == "number")
		{
			langutil::CharStream charStream(value, "");
			langutil::Scanner scanner{charStream};
			lit.kind = LiteralKind::Number;
			yulAssert(
				scanner.currentToken() == Token::Number,
				"Expected number but got " + langutil::TokenTraits::friendlyName(scanner.currentToken()) + std::string(" while scanning ") + value
			);
		}
		else if (kind == "bool")
		{
			langutil::CharStream charStream(value, "");
			langutil::Scanner scanner{charStream};
			lit.kind = LiteralKind::Boolean;
			yulAssert(
				scanner.currentToken() == Token::TrueLiteral ||
				scanner.currentToken() == Token::FalseLiteral,
				"Expected true/false literal!"
			);
		}
		else if (kind == "string")
		{
			lit.kind = LiteralKind::String;
			yulAssert(
				value.size() <= 32,
				"String literal too long (" + std::to_string(value.size()) + " > 32)"
			);
		}
		else
			yulAssert(false, "unknown type of literal");

		// import only for inline assembly, no unlimited string literals there
		lit.value = valueOfLiteral(value, lit.kind, false /* _unlimitedLiteralArgument */);

		yulAssert(validLiteral(lit));
		return lit;
	}

	Expression fromDto(ast_json::YulExpression const& _dto) const
	{
		return _dto.value.visit([this](auto const& _alt) -> Expression { return fromDto(_alt); });
	}

	Statement fromDto(ast_json::YulStatement const& _dto) const
	{
		return _dto.value.visit([this](auto const& _alt) -> Statement { return fromDto(_alt); });
	}

	FunctionCall fromDto(ast_json::YulFunctionCall const& _dto) const
	{
		auto functionCall = createAsmNode<FunctionCall>(_dto.src);
		functionCall.arguments.reserve(_dto.arguments.size());
		for (auto const& arg: _dto.arguments)
			functionCall.arguments.emplace_back(fromDto(arg));

		if (std::optional<BuiltinHandle> builtinHandle = dialect.findBuiltin(_dto.functionName.name))
		{
			auto builtin = createAsmNode<BuiltinName>(_dto.functionName.src);
			builtin.handle = *builtinHandle;
			functionCall.functionName = builtin;
		}
		else
			functionCall.functionName = fromDto(_dto.functionName);

		return functionCall;
	}

	Block fromDto(ast_json::YulBlock const& _dto) const
	{
		auto block = createAsmNode<Block>(_dto.src);
		block.statements.reserve(_dto.statements.size());
		for (auto const& stmt: _dto.statements)
			block.statements.emplace_back(fromDto(stmt));
		return block;
	}

	ExpressionStatement fromDto(ast_json::YulExpressionStatement const& _dto) const
	{
		auto statement = createAsmNode<ExpressionStatement>(_dto.src);
		statement.expression = fromDto(_dto.expression);
		return statement;
	}

	Assignment fromDto(ast_json::YulAssignment const& _dto) const
	{
		auto assignment = createAsmNode<Assignment>(_dto.src);
		if (_dto.variableNames)
		{
			assignment.variableNames.reserve(_dto.variableNames->size());
			for (auto const& var: *_dto.variableNames)
				assignment.variableNames.emplace_back(fromDto(var));
		}
		yulAssert(_dto.value.get().value.has_value(), "Expected expression for YulAssignment value.");
		assignment.value = std::make_unique<Expression>(fromDto(*_dto.value.get().value));
		return assignment;
	}

	VariableDeclaration fromDto(ast_json::YulVariableDeclaration const& _dto) const
	{
		auto varDec = createAsmNode<VariableDeclaration>(_dto.src);
		if (_dto.variables)
		{
			varDec.variables.reserve(_dto.variables->size());
			for (auto const& var: *_dto.variables)
				varDec.variables.emplace_back(fromDto(var));
		}
		if (_dto.value.get().value)
			varDec.value = std::make_unique<Expression>(fromDto(*_dto.value.get().value));
		return varDec;
	}

	FunctionDefinition fromDto(ast_json::YulFunctionDefinition const& _dto) const
	{
		auto funcDef = createAsmNode<FunctionDefinition>(_dto.src);
		funcDef.name = YulName{_dto.name};
		if (_dto.parameters)
		{
			funcDef.parameters.reserve(_dto.parameters->size());
			for (auto const& param: *_dto.parameters)
				funcDef.parameters.emplace_back(fromDto(param));
		}
		if (_dto.returnVariables)
		{
			funcDef.returnVariables.reserve(_dto.returnVariables->size());
			for (auto const& retVar: *_dto.returnVariables)
				funcDef.returnVariables.emplace_back(fromDto(retVar));
		}
		funcDef.body = fromDto(_dto.body);
		return funcDef;
	}

	If fromDto(ast_json::YulIf const& _dto) const
	{
		auto ifStatement = createAsmNode<If>(_dto.src);
		ifStatement.condition = std::make_unique<Expression>(fromDto(_dto.condition));
		ifStatement.body = fromDto(_dto.body);
		return ifStatement;
	}

	Case fromDto(ast_json::YulCase const& _dto) const
	{
		auto caseStatement = createAsmNode<Case>(_dto.src);
		rfl::visit(
			[&](auto const& _val)
			{
				using T = std::decay_t<decltype(_val)>;
				if constexpr (std::is_same_v<T, ast_json::YulLiteral>)
					caseStatement.value = std::make_unique<Literal>(fromDto(_val));
			},
			_dto.value
		);
		caseStatement.body = fromDto(_dto.body);
		return caseStatement;
	}

	Switch fromDto(ast_json::YulSwitch const& _dto) const
	{
		auto switchStatement = createAsmNode<Switch>(_dto.src);
		switchStatement.expression = std::make_unique<Expression>(fromDto(_dto.expression));
		if (_dto.cases)
		{
			switchStatement.cases.reserve(_dto.cases->size());
			for (auto const& c: *_dto.cases)
				switchStatement.cases.emplace_back(fromDto(c));
		}
		return switchStatement;
	}

	ForLoop fromDto(ast_json::YulForLoop const& _dto) const
	{
		auto forLoop = createAsmNode<ForLoop>(_dto.src);
		forLoop.pre = fromDto(_dto.pre);
		forLoop.condition = std::make_unique<Expression>(fromDto(_dto.condition));
		forLoop.post = fromDto(_dto.post);
		forLoop.body = fromDto(_dto.body);
		return forLoop;
	}

	Break fromDto(ast_json::YulBreak const& _dto) const
	{
		return createAsmNode<Break>(_dto.src);
	}

	Continue fromDto(ast_json::YulContinue const& _dto) const
	{
		return createAsmNode<Continue>(_dto.src);
	}

	Leave fromDto(ast_json::YulLeave const& _dto) const
	{
		return createAsmNode<Leave>(_dto.src);
	}
};

} // anonymous namespace

AST AsmJsonImporter::createAST(solidity::Json const& _node)
{
	auto parsed = rfl::nlohmann::read<ast_json::YulBlock>(_node);
	yulAssert(parsed.has_value(), parsed.error().what());
	return {m_dialect, AstBuilder{m_dialect, m_sourceNames}.fromDto(*parsed)};
}

}
