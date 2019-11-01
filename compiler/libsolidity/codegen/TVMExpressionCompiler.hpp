/*
 * Copyright 2018-2019 TON DEV SOLUTIONS LTD.
 *
 * Licensed under the  terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the  GNU General Public License for more details at: https://www.gnu.org/licenses/gpl-3.0.html
 */
/**
 * @author TON Labs <connect@tonlabs.io>
 * @date 2019
 * Expression compiler for TVM
 */

#pragma once

#include "TVMCommons.hpp"
#include "TVMIntrinsics.hpp"
#include "TVMFunctionCall.hpp"

namespace dev {
namespace solidity {

class TVMExpressionCompiler : public IExpressionCompiler, private StackPusherHelper {
private:
	ITVMCompiler* const m_compiler;
	int m_expressionDepth;

public:
	TVMExpressionCompiler(ITVMCompiler* compiler, const TVMCompilerContext& ctx) 
		: StackPusherHelper(compiler, &ctx)
		, m_compiler(compiler)
		, m_expressionDepth{0} 
	{
	}

	void acceptExpr2(const Expression* expr, const bool isResultNeeded = true) override {
		solAssert(expr, "");
		// Recursive call are not allowed.
		solAssert(m_expressionDepth == 0, "");
		m_expressionDepth = isResultNeeded ? 1 : 0;
		acceptExpr(expr);
		m_expressionDepth = 0;
	}

protected:
	void acceptExpr(const Expression* expr) override {
		const int savedExpressionDepth = m_expressionDepth;
		++m_expressionDepth;
		if (auto e = to<Literal>(expr)) {
			visit2(*e);
		} else if (auto e = to<Identifier>(expr)) {
			visit2(*e);
		} else if (auto e = to<BinaryOperation>(expr)) {
			visit2(*e);
		} else if (auto e = to<UnaryOperation>(expr)) {
			visit2(*e);
		} else if (auto e = to<Assignment>(expr)) {
			visit2(*e);
		} else if (auto e = to<TupleExpression>(expr)) {
			visit2(*e);
		} else if (auto e = to<MemberAccess>(expr)) {
			visit2(*e);
		} else if (auto e = to<IndexAccess>(expr)) {
			visit2(*e);
		} else if (auto e = to<FunctionCall>(expr)) {
			visit2(*e);
		} else if (auto e = to<Conditional>(expr)) {
			visit2(*e);
		} else if (auto e = to<ElementaryTypeNameExpression>(expr)) {
			visit2(*e);
		} else {
			cast_error(*expr, string("Unsupported expression ") + typeid(expr).name());
		}
		--m_expressionDepth;
		solAssert(savedExpressionDepth == m_expressionDepth, 
				  "Internal error: depth exp " + toString(savedExpressionDepth) + " got " + toString(m_expressionDepth));
	}

protected:
	void visit2(Literal const& _node) {
		const auto* type = getType(&_node);
		Type::Category category =_node.annotation().type->category();
		if (category == Type::Category::Integer ||
			(to<RationalNumberType>(type) && !to<RationalNumberType>(type)->isFractional()) ||
			category == Type::Category::Address) {

			dev::u256 value = type->literalValue(&_node);
			push(+1, "PUSHINT " + toString(value));
		} else if (category == Type::Category::Bool) {
			push(+1, _node.token() == Token::TrueLiteral? "TRUE" : "FALSE");
		} else {
			cast_error(_node, string("Unsupported literal type ") + type->canonicalName());
		}
	}

	void visit2(TupleExpression const& _tupleExpression) {
		for (auto comp : _tupleExpression.components()) {
			acceptExpr(comp.get());
		}
	}

	bool pushMemberOrLocalVar(Identifier const& _identifier) {
		string name = _identifier.name();
		auto& stack = getStack();
		if (stack.isParam(name)) {
			// push(0, string(";; ") + m_stack.dumpParams());
			auto offset = stack.getOffset(name);
			if (offset == 0) {
				push(+1, "DUP");
			} else {
				push(+1, "PUSH s" + toString(offset));
			}
			return true;
		} else if (auto idx = ctx().getMemberIdx(name); idx >= 0) {
			pushInt(idx);
			pushPersistentDataDict();
			getFromDict(getType(&_identifier), TvmConst::C4::KeyLength);
			return true;
		}
		return false;
	}

protected:
	void visit2(Identifier const& _identifier) {
		const string& name = _identifier.name();
		push(0, string(";; ") + name);
		dumpStackSize();
		if (pushMemberOrLocalVar(_identifier)) {
		} else if (name == "now") {
			// Getting value of `now` variable
			push(+1, "NOW");
		} else if (ctx().getLocalFunction(name)) {
			CodeLines code;
			code.push("CALL $" + name + "_internal$");
			pushCont(code);
		} else {
			cast_error(_identifier, "Unsupported identifier: " + name);
		}
	}

	void compileUnaryOperation(UnaryOperation const& _node, std::string tvmUnaryOperation) {
		if (m_expressionDepth > 1) {
			cast_error(_node, "++ operation is supported only in simple expressions.");
		}

		if (auto identifier = to<Identifier>(&_node.subExpression())) {
			auto name = identifier->name();
			TVMStack& stack = getStack();
			if (stack.isParam(name) && stack.getOffset(name) == 0) {
				push(0, tvmUnaryOperation);
				return;
			}
		}

		if (const std::vector<Expression const*> exprs = unrollLHS(&_node.subExpression());
			exprs[0] != nullptr) {
			expandMappingValue(exprs, true);
			push(0, tvmUnaryOperation);
			collectMappingValue(exprs);
			return ;
		}

		cast_error(_node, "++ operation is supported only for simple or mapping variables.");
	}

	void compileUnaryDelete(UnaryOperation const& node) {
		const std::vector<Expression const*> exprs = unrollLHS(&node.subExpression());
		if (exprs[0] == nullptr) {
			cast_error(node, "delete operation is supported only for simple or mapping variables.");
		}

		if (to<Identifier>(exprs.back())) {
			expandMappingValue(exprs, false);
			Type const* exprType = getType(&node.subExpression());
			if (to<AddressType>(exprType)) {
				pushInt(0);
				pushPrivateFunctionCall(-1 + 1, "make__MsgAddressInt__addr_std_10");
				push(0, "ENDC");
				push(0, "CTOS");
			} else if (to<IntegerType>(exprType) ||
				to<BoolType>(exprType) ||
				to<FixedBytesType>(exprType)) {
				pushInt(0);
			} else if (auto arrayType = to<ArrayType>(exprType)) {
				if (arrayType->isDynamicallySized()) {
					push(+1, "NEWDICT");
				} else {
					cast_error(node, "delete operation is not supported for statically-sized arrays.");
				}
			} else if (to<StructType>(exprType)) {
				push(+1, "NEWDICT");
			} else {
				cast_error(node, "delete operation is not supported for " + exprType->toString());
			}
			collectMappingValue(exprs);
		} else if (to<MemberAccess>(exprs.back())) {
			expandMappingValue(exprs, false);    // ... index dict
			push(+1, "PUSH S1");                 // ... index dict index
			push(0, "SWAP");                     // ... index index dict
			pushInt(8);                          // ... index index dict 8
			push(-3+2, "DICTUDEL");              // ... index dict' {-1,0}
			push(-1, "DROP");                    // ... index dict'
			collectMappingValue(exprs, false);
		} else if (auto indexAccess = to<IndexAccess>(exprs.back())) {
			expandMappingValue(exprs, false);               // ... index dict
			push(+1, "PUSH S1");                            // ... index dict index
			push(0, "SWAP");                                // ... index index dict
			auto [numBits, isSigned, _] = parseIndexType(indexAccess);
			_ = _; // to suppress "unused" error
			pushInt(numBits);                               // ... index index dict numBits
			push(-3+2, isSigned? "DICTIDEL" : "DICTUDEL");  // ... index dict' {-1,0}
			push(-1, "DROP");                               // ... index dict'
			collectMappingValue(exprs, false);
		} else {
			solAssert(false, "");
		}
	}

	void visit2(UnaryOperation const& _node) {
		auto op = _node.getOperator();
		push(0, string(";; ") + TokenTraits::toString(op));
		if (op == Token::Inc) {
			compileUnaryOperation(_node, "INC");
		} else if (op == Token::Dec) {
			compileUnaryOperation(_node, "DEC");
		} else if (op == Token::Not) {
			acceptExpr(&_node.subExpression());
			push(0, "NOT");
		} else if (op == Token::Sub) {
			acceptExpr(&_node.subExpression());
			push(0, "NEGATE");
		} else if (op == Token::BitNot) {
			acceptExpr(&_node.subExpression());
			int numBits = 0;
			bool isSigned = false;
			auto type = getType(&_node.subExpression());
			TypeInfo ti = TypeInfo(type);
			if (ti.isNumeric) {
				numBits = ti.numBits;
				isSigned = ti.isSigned;
			} else {
				cast_error(_node, "~ operation is supported only for numbers.");
			}
			if (isSigned)
				push(0, "NOT");
			else {
				push(0,"UFITS " + to_string(numBits));
				push(+1,"PUSHPOW2DEC " + to_string(numBits));
				push(-2+1, "SUBR");
			}
		} else if (op == Token::Delete) {
			compileUnaryDelete(_node);
		} else {
			cast_error(_node, toString("Unsupported operation: ") + TokenTraits::toString(op));
		}
	}

	static bool checkArgumentsForFixedBytes(Type const* a, Type const* b) {
		Type::Category lh = a->category();
		Type::Category rh = b->category();
		if ((lh == Type::Category::FixedBytes || rh == Type::Category::FixedBytes) && lh != rh) {
			return false;
		} else if (lh == Type::Category::FixedBytes && rh == Type::Category::FixedBytes) {
			if (to<FixedBytesType>(a)->storageBytes() != to<FixedBytesType>(b)->storageBytes())
				return false;
		}
		return true;
	}

	string compareAddresses(Token op) {
		if (op == Token::GreaterThan)
			return "SDLEXCMP ISPOS";
		if (op == Token::GreaterThanOrEqual)
			return "SDLEXCMP ISNNEG";
		if (op == Token::LessThan)
			return "SDLEXCMP ISNEG";
		if (op == Token::LessThanOrEqual)
			return "SDLEXCMP ISNPOS";
		if (op == Token::Equal)
			return "SDEQ";
		if (op == Token::NotEqual)
			return "SDEQ NOT";
		solAssert(false, "Wrong compare operation");
	}

	void visit2(BinaryOperation const& _node) {
		if (!checkArgumentsForFixedBytes(getType(&_node.leftExpression()), getType(&_node.rightExpression()))) {
			cast_error(_node, "Unsupported operation for this arguments");
		}

		Type::Category leftType = _node.leftExpression().annotation().type->category();
		Type::Category rightType = _node.leftExpression().annotation().type->category();

		acceptExpr(&_node.leftExpression());
		acceptExpr(&_node.rightExpression());
		push(0, string(";; ") + TokenTraits::toString(_node.getOperator()));
		auto op = _node.getOperator();
		if (op == Token::Exp) {
			// TODO: this code is hard to understand. Would it be better to move it to stdlib?
			push(0, "SWAP");
			push(0, "PUSHINT 1");
			push(0, "PUSHCONT {");
			push(0, "\tPUSH s2");
			push(0, "\tPUSHINT 0");
			push(0, "\tGREATER");
			push(0, "\tNOT DUP IFRET DROP");
			push(0, "\tPUSH s2");
			push(0, "\tPUSHINT 1");
			push(0, "\tAND");
			push(0, "\tPUSHINT 1");
			push(0, "\tEQUAL");
			push(0, "\tPUSHCONT {");
			push(0, "\t\tDUP");
			push(0, "\t\tPUSH s2");
			push(0, "\t\tMUL");
			push(0, "\t\tNIP");
			push(0, "\t}");
			push(0, "\tIF");
			push(0, "\tPUSH s2");
			push(0, "\tPUSHINT 1");
			push(0, "\tRSHIFT");
			push(0, "\tPOP s3");
			push(0, "\tPUSH s1");
			push(0, "\tPUSH s2");
			push(0, "\tMUL");
			push(0, "\tPOP s2");
			push(0, "\tFALSE");
			push(0, "}");
			push(0, "UNTIL");
			push(0, "NIP NIP");	// remove operands
			push(-2+1, "");	// fixup stack
			return;
		}
		
		if ((op >= Token::Equal) && (op <= Token::GreaterThanOrEqual)) {
			if (leftType  == Type::Category::Address || rightType  == Type::Category::Address) {
				if (leftType  != Type::Category::Address || rightType  != Type::Category::Address) {
					cast_error(_node, "Can't compare address type and not address type");
				}
				push(-1, compareAddresses(op));
				return;
			}
		}
		 
		if (op == Token::SHR) cast_error(_node, "Unsupported operation >>>");
		if (op == Token::Comma) cast_error(_node, "Unsupported operation ,");
		string cmd = "???";
		if (op == Token::Add) cmd = "ADD";
		if (op == Token::Mul) cmd = "MUL";
		if (op == Token::Sub) cmd = "SUB";
		if (op == Token::Mod) cmd = "MOD";
		if (op == Token::Div) cmd = "DIV";
		if (op == Token::GreaterThan) cmd = "GREATER";
		if (op == Token::GreaterThanOrEqual) cmd = "GEQ";
		if (op == Token::LessThan) cmd = "LESS";
		if (op == Token::LessThanOrEqual) cmd = "LEQ";
		if (op == Token::Equal) cmd = "EQUAL";
		if (op == Token::NotEqual) cmd = "NEQ";
		if (op == Token::And) cmd = "AND";
		if (op == Token::Or) cmd = "OR";
		if (op == Token::SHL) cmd = "LSHIFT";
		if (op == Token::SAR) cmd = "RSHIFT";
		if (op == Token::BitAnd) cmd = "AND";
		if (op == Token::BitOr) cmd = "OR";
		if (op == Token::BitXor) cmd = "XOR";
		push(-1, cmd);
	}
	
	bool checkForMagic(MemberAccess const& _node, Type::Category category) {
		if (category != Type::Category::Magic)
			return false;
		auto expr = to<Identifier>(&_node.expression());
		if (expr != nullptr && expr->name() == "msg") {
			if (_node.memberName() == "sender") { // msg.sender
				push(+1, "PUSHCTR c7");
				push(0, "SECOND");
				push(0, "SECOND");
				return true;
			}
			if (_node.memberName() == "value") { // msg.value
				push(+1, "PUSHCTR c7");
				push(0, "SECOND");
				push(0, "THIRD");
				return true;
			}
		}
		cast_error(_node, "Unsupported magic");
		return false;
	}
	
	void visit2(MemberAccess const& _node) {
		std::string name = _node.memberName();
		push(0, ";; get member " + name);
		auto category = getType(&_node.expression())->category();
		if (category == Type::Category::Struct) {
			auto type = pushStructMemberIndex(&_node);
			acceptExpr(&_node.expression());
			getFromDict(type, 8);
			return;
		}

		if (checkForMagic(_node, category)) return; 
		if (checkForMemberAccessBalance(_node, category)) return;
		
		if (category == Type::Category::Array) 
			return visitMemberAccessArray(_node);

		if (category == Type::Category::FixedBytes)
			return visitMemberAccessFixedBytes(_node, to<FixedBytesType>(getType(&_node.expression())));
		
		cast_error(_node, "Not supported");
	}
	
	bool checkForMemberAccessBalance(MemberAccess const& _node, Type::Category category) {
		if (category != Type::Category::Address)
			return false;
		if (_node.memberName() == "balance") {
			if (!isAddressThis(to<FunctionCall>(&_node.expression())))
				cast_error(_node.expression(), "only 'address(this).balance' is supported for balance");
			pushPrivateFunctionCall(+1, "get_contract_balance");
			return true;
		}
		return false;
	}

	void visitMemberAccessArray(MemberAccess const& _node) {
		if (_node.memberName() == "length") {
			acceptExpr(&_node.expression());
			pushInt(32);
			push(-2+3, "DICTUMAX");
			push(+1, "PUSHCONT { POP s1 INC }");
			push(+1, "PUSHCONT { PUSHINT 0 }");
			push(-3-1, "IFELSE");
			return;
		}
		cast_error(_node, "Unsupported 489");
	}

	void visitMemberAccessFixedBytes(MemberAccess const& _node, FixedBytesType const* fbt) {
		if (_node.memberName() == "length" && to<Identifier>(&_node.expression())) {
			pushInt(fbt->storageBytes());
			return;
		}
		cast_error(_node, "Unsupported");
	}

	static std::tuple<int, bool, Type::Category> parseIndexType(IndexAccess const* index) {
		int numBits = 0;
		bool isSigned = false;
		Type::Category keyType = Type::Category::Integer;
		auto type = getType(&index->baseExpression());
		if (to<ArrayType>(type)) {
			numBits = 32;
			isSigned = false;
			keyType = Type::Category::Integer;
		} else if (auto mappingType = to<MappingType>(type)) {
			auto t = mappingType->keyType().get();
			TypeInfo ti {t};
			if (ti.isNumeric) {
				numBits = ti.numBits;
				isSigned = ti.isSigned;
				keyType = ti.category;
			} else {
				solAssert(false, string("Unsupported index type: ") + typeid(*t).name());
			}
		}
		return {numBits, isSigned, keyType};
	}

	void visit2(IndexAccess const& _node) {
		Type::Category baseExprCategory = _node.baseExpression().annotation().type->category();
		if (baseExprCategory != Type::Category::Array &&
			baseExprCategory != Type::Category::Mapping) {
			cast_error(_node, "Index access is supported only for dynamic arrays and mappings");
		}
		acceptExpr(_node.indexExpression());
		acceptExpr(&_node.baseExpression());
		push( 0, ";; index");
		auto [numBits, isSigned, keyCategory] = parseIndexType(&_node);
		getFromDict(getType(&_node), numBits, isSigned, keyCategory);
	}

	bool checkAbiMethodCall(FunctionCall const& _functionCall) {
		// compile "abi.encodePacked()"
		if (auto memberAccess = to<MemberAccess>(&_functionCall.expression())) {
			auto identifier = to<Identifier>(&memberAccess->expression());
			if (identifier != nullptr && identifier->name() == "abi") {
				if (memberAccess->memberName() != "encodePacked") {
					cast_error(_functionCall, "Only method encodePacked is supported for abi");
				}
				// TODO check builder overflow?
				push(+1, "NEWC");
				for (const ASTPointer<Expression const>& argument : _functionCall.arguments()) {
					const Type *type = getType(argument.get());
					if (to<IntegerType>(type) != nullptr) {
						acceptExpr(argument.get());
						push(-1, makeStoreTypeCommand(type, true));
					} else if (auto array = to<ArrayType>(type)) {
						if (to<IntegerType>(array->baseType().get()) == nullptr) {
							cast_error(*argument.get(), "Only numeric array is supported for abi.encodePacked(...) ");
						}
						if (_functionCall.arguments().size() != 1) {
							cast_error(_functionCall, "Only one array is supported for abi.encodePacked(...)");
						}
						TypeInfo arrayElementTypeInfo(array->baseType().get());
						acceptExpr(argument.get());
						pushInt(arrayElementTypeInfo.numBits);
						pushPrivateFunctionCall(-3 + 1,"abi_encode_packed");
					} else {
						cast_error(_functionCall, "Only numeric types or numeric arrays are supported for abi.encodePacked(...)");
					}
				}
				push(0, "ENDC CTOS");
				// DOTO return bytes?
				return true;
			}
		}
		return false;
	}

public:
	void encodeOutboundMessageBody(
			const string& name,
			const ptr_vec<Expression const>&	arguments,
			const ptr_vec<VariableDeclaration>&	parameters,
			const StackPusherHelper::ReasonOfOutboundMessage reason)
	{
		solAssert(parameters.size() == arguments.size(), "");
		auto& stack = getStack();
		auto savedStackSize = stack.size();

		std::vector<Type const*> types;
		std::vector<ASTNode const*> nodes;
		for (const auto & argument : parameters) {
			types.push_back(argument->annotation().type.get());
			nodes.push_back(argument.get());
		}

		encodeFunctionAndParams(
				name,
				types,
				nodes,
				[&](size_t idx) {
					push(0, ";; " + parameters[idx]->name());
					acceptExpr(arguments[idx].get());
				},
				reason
		);

		stack.ensureSize(savedStackSize + 1, "encodeRemoteFunctionCall");
	}

protected:
	bool checkRemoteMethodCallWithValue(FunctionCall const& _functionCall) {
		// TODO: simplify this code
		const ptr_vec<Expression const> arguments = _functionCall.arguments();
		if (auto functionValue = to<FunctionCall>(&_functionCall.expression())) {
			auto argumentsValue = functionValue->arguments();
			if (argumentsValue.size() != 1)
				return false;
			if (auto memberValue = to<MemberAccess>(&functionValue->expression())) {
				if (memberValue->memberName() != "value")
					return false;
				if (auto memberAccess = to<MemberAccess>(&memberValue->expression())) {
					acceptExpr(argumentsValue[0].get());
					acceptExpr(&memberAccess->expression());
					if (const FunctionDefinition* fdef = m_compiler->getRemoteFunctionDefinition(memberAccess)) {
						auto fn = ctx().getFunctionExternalName(memberAccess->memberName());
						if (m_expressionDepth > 1)
							cast_error(_functionCall, "Calls to remote contract do not return result.");
						encodeOutboundMessageBody(fn, arguments, fdef->parameters(), StackPusherHelper::ReasonOfOutboundMessage::RemoteCallInternal);
						pushPrivateFunctionCall(-3, "send_grams");
						dumpStackSize();
						return true;
					}
				}
			}
		}
		return false;
	}

	bool checkRemoteMethodCall(FunctionCall const& _functionCall) {
		auto arguments = _functionCall.arguments();
		if (auto memberAccess = to<MemberAccess>(&_functionCall.expression())) {
			if (const FunctionDefinition* fdef = m_compiler->getRemoteFunctionDefinition(memberAccess)) {
				acceptExpr(&memberAccess->expression());
				auto fn = ctx().getFunctionExternalName(memberAccess->memberName());
				if (m_expressionDepth > 1)
					cast_error(_functionCall, "Calls to remote contract do not return result.");
				encodeOutboundMessageBody(fn, arguments, fdef->parameters(), StackPusherHelper::ReasonOfOutboundMessage::RemoteCallInternal);
				pushPrivateFunctionCall(-2, "send_int_msg_2");
				return true;
			}
		}
		return false;
	}
	
	void visit2(FunctionCall const& _functionCall) {
		auto arguments = _functionCall.arguments();

		FunctionCallCompiler fcc(m_compiler, this, ctx());
		
		if (checkAbiMethodCall(_functionCall)) return;
		if (checkRemoteMethodCall(_functionCall)) return;
		if (checkRemoteMethodCallWithValue(_functionCall)) return;

		fcc.compile(_functionCall);
	}
		
	void visit2(Conditional const& _conditional) {
		CodeLines codeTrue  = m_compiler->proceedContinuationExpr(_conditional.trueExpression());
		CodeLines codeFalse = m_compiler->proceedContinuationExpr(_conditional.falseExpression());
		acceptExpr(&_conditional.condition());
		// TODO: no need to call compiler here
		m_compiler->applyContinuation(codeTrue);
		m_compiler->applyContinuation(codeFalse);
		push(-3+1, "IFELSE");
	}

	void visit2(ElementaryTypeNameExpression const& _node) {
		ensureValueFitsType(_node.typeName());
	}

	static std::vector<Expression const*> unrollLHS(Expression const* expr) {
		std::vector<Expression const*> res;
		if (to<Identifier>(expr)) {
			res.push_back(expr);
			return res;
		}
		if (auto index = to<IndexAccess>(expr)) {
			res = unrollLHS(&index->baseExpression());
			res.push_back(expr);
			return res;
		}
		if (auto index = to<MemberAccess>(expr)) {
			if (getType(&index->expression())->category() == Type::Category::Struct) {
				res = unrollLHS(&index->expression());
				res.push_back(expr);
				return res;
			}
		}
		res.push_back(nullptr);
		return res;
	}
	
	Type const* pushStructMemberIndex(MemberAccess const* memberAccess, bool doPush = true) {
		auto structType = to<StructType>(memberAccess->expression().annotation().type.get());
		const string structName = structType->structDefinition().name();
		const string memberName = memberAccess->memberName();

		Type const* memberType = nullptr;
		int index = 0;
		for (const ASTPointer<VariableDeclaration>& member :  structType->structDefinition().members()) {
			if (member->name() == memberName) {
				memberType = member->type().get();
				break;
			}
			++index;
		}
		solAssert(memberType != nullptr, "");

		if (doPush) {
			push(0, ";; Index of " + structName + "." + memberName);
			pushInt(index);
		}
		return memberType;
	}
	
protected:

	void expandMappingValue(const std::vector<Expression const*> &exprs, const bool withExpandLastValue) {
		for (size_t i = 0; i < exprs.size(); i++) {
			bool isLast = (i + 1) == exprs.size();
			if (auto variable = to<Identifier>(exprs[i])) {
				if (isLast && !withExpandLastValue) break;
				auto name = variable->name();
				push(0, ";; fetch " + name);
				if (!pushMemberOrLocalVar(*variable)) {
					solAssert(false, string("Invalid assignment - ") + name);
				}
			}
			else if (auto index = to<IndexAccess>(exprs[i])) {
				// dict1
				acceptExpr(index->indexExpression());
				// dict1 index
				push(0, "SWAP");
				// index dict1
				if (isLast && !withExpandLastValue) break;
				push(+2, "PUSH2 S1, S0");
				// index dict1 index dict1

				auto[numBits, isSigned, keyCategory] = parseIndexType(index);
				getFromDict(getType(index), numBits, isSigned, keyCategory);
				// index dict1 dict2
			}
			else if (auto index = to<MemberAccess>(exprs[i])) {
				// dict1
				auto type = pushStructMemberIndex(index);
				// dict1 index
				push(0, "SWAP");
				// index dict1
				if (isLast && !withExpandLastValue) break;
				push(+2, "PUSH2 S1, S0");
				// index dict1 index dict1
				getFromDict(type, 8);
				// index dict1 dict2
			} else {
				solAssert(false, "");
			}
		}
	}

	void collectMappingValue(const std::vector<Expression const*> &exprs, const bool haveValueOnStackTop = true) {
		// if haveValueOnStackTop then stacktrace: [index dict value] for {IndexAccess,MemberAccess} or [value] for Identifier
		// else                        stacktrace: [index dict]       for {IndexAccess,MemberAccess} or []      for Identifier
		for (int i = exprs.size() - 1; i >= 0; i--) {
			bool isLast = (i + 1) == (int)exprs.size();
			if (auto variable = to<Identifier>(exprs[i])) {
				if (isLast && !haveValueOnStackTop)
					continue;
				assignVar(variable);
			} else if (auto index = to<IndexAccess>(exprs[i])) {
				if (isLast && !haveValueOnStackTop) {
					push(-1, "NIP");
				} else {
					auto type = getType(index);
					if (isLast) {
						if (isIntegralType(type)) {
							intToBuilder(type);
						}
					}

					auto[numBits, isSigned, keyCategory] = parseIndexType(index);

					// index dict1 index2 dict2 value
					push(0, "ROTREV");
					// index dict1 value index2 dict2
					setDict(isIntegralType(type), numBits, isSigned, keyCategory);
					// index dict1 dict2'
				}
			} else if (auto index = to<MemberAccess>(exprs[i])) {
				if (isLast && !haveValueOnStackTop) {
					push(-1, "NIP");
				} else {
					if (isLast) {
						// TODO: check other types
						auto type = pushStructMemberIndex(index, false);
						if (isIntegralType(getType(index)))
							intToBuilder(type);
					}
					// index dict1 index2 dict2 value
					push(0, "ROTREV");
					// index dict1 value index2 dict2
					setDict(isIntegralType(getType(index)), 8, false);
					// index dict1 dict2'
				}
			} else {
				solAssert(false, "");
			}
		}
	}

	bool tryAssignMapping(Assignment const& _assignment) {
		Type::Category leftType = _assignment.leftHandSide().annotation().type->category();
		const vector<Expression const*> exprs = unrollLHS(&_assignment.leftHandSide());
		if (!exprs[0])
			return false;

		if (m_expressionDepth > 1) {
			cast_error(_assignment, "Assignment is supported only in simple expressions.");
		}
		const Token op = _assignment.assignmentOperator();
		if (op == Token::Assign){
			if (leftType == Type::Category::Address && to<Literal>(&_assignment.rightHandSide())) {
				cast_error(_assignment, "Can't convert literal to address. Use tvm_make_address");
			}
			expandMappingValue(exprs, false);
			acceptExpr(&_assignment.rightHandSide());
		} else {
			string cmd;
			if      (op == Token::AssignAdd)    cmd = "ADD";
			else if (op == Token::AssignMul)    cmd = "MUL";
			else if (op == Token::AssignSub)    cmd = "SUB";
			else if (op == Token::AssignMod)    cmd = "MOD";
			else if (op == Token::AssignDiv)    cmd = "DIV";
			else if (op == Token::AssignBitAnd) cmd = "AND";
			else if (op == Token::AssignBitOr)  cmd = "OR";
			else if (op == Token::AssignBitXor) cmd = "XOR";
			else if (op == Token::AssignShl)    cmd = "LSHIFT";
			else if (op == Token::AssignSar)    cmd = "RSHIFT";
			else {
				cast_error(_assignment, "Unsupported operation.");
			}

			expandMappingValue(exprs, true);
			acceptExpr(&_assignment.rightHandSide());
			push(-1, cmd);
		}

		collectMappingValue(exprs);
		return true;
	}

	bool tryAssign(const TupleExpression * lhs, const TupleExpression * rhs) {
		for (size_t i = 0; i < lhs->components().size(); i++) {
			const vector<Expression const*> exprs = unrollLHS(lhs->components()[i].get());
			if (!exprs[0])
				return false;
			expandMappingValue(exprs, false);
			acceptExpr(rhs->components()[i].get());
			collectMappingValue(exprs);
		}
		return true;
	}

	bool tryAssignTuple(Assignment const& _assignment) {
		if (auto lhs = to<TupleExpression>(&_assignment.leftHandSide())) {
			if (auto rhs = to<TupleExpression>(&_assignment.rightHandSide())) {
				if (lhs->components().size() != rhs->components().size())
					cast_error(_assignment, "Tuples in assignment should be the same size.");
				if (_assignment.assignmentOperator() != Token::Assign)
					cast_error(_assignment, "Unsupported operation.");
				return tryAssign(lhs, rhs);
			} else if (auto rhs = to<FunctionCall>(&_assignment.rightHandSide())) {
				visit2(*rhs);
				push(0, "REVERSE " + to_string(lhs->components().size()) + ", 0");
				for (size_t i = 0; i < lhs->components().size(); i++) {
					if (!lhs->components()[i]) {
						push(-1, "DROP");
						continue;
					}
					const vector<Expression const*> exprs = unrollLHS(lhs->components()[i].get());
					if (!exprs[0])
						cast_error(_assignment, "Unsupported tuple field.");
					expandMappingValue(exprs, false);
					if ((to<MemberAccess>(lhs->components()[i].get())) ||
						 (to<IndexAccess>(lhs->components()[i].get()))){
						push(0, "BLKSWAP 1, 2");
					}
					collectMappingValue(exprs);
				}
				return true;
			}
		}
		return false;
	}


	bool tryAssignArrayLength(Assignment const& _assignment) {
		if (auto index = to<MemberAccess>(&_assignment.leftHandSide());
			index != nullptr &&
			index->memberName() == "length") {

			auto arrayType = to<ArrayType>(getType(&index->expression()));
			if (arrayType == nullptr) {
				return false; // maybe it's struct
			}

			const Token &op = _assignment.assignmentOperator();
			if (op != Token::Assign) {
				cast_error(_assignment, "Only simple assignment supported for array.length.");
			}

			const std::vector<Expression const*> exprs = unrollLHS(&index->expression());
			if (!exprs[0])
				return false;

			expandMappingValue(exprs, true);
			// some_expanded_data... arr

			auto arrayBaseType = arrayType->baseType().get();
			int numBits = TypeInfo(arrayBaseType).numBits;
			if (numBits == 0) {
				// TODO: Fix an issue#2 with arr.length
				push(+1, "NEWDICT");
			} else {
				push(+1, "PUSHINT 0");
				push(-1+1, "NEWC STU " + toString(numBits));
			}
			// some_expanded_data... arr defaultValue

			push(0, "SWAP");
			// some_expanded_data... defaultValue arr

			acceptExpr(&_assignment.rightHandSide());
			// some_expanded_data... defaultValue arr newArrSize

			pushPrivateFunctionCall(-3 + 1,    "change_array_length");
			// some_expanded_data arr'

			collectMappingValue(exprs);
			return true;
		}
		return false;
	}

	void visit2(Assignment const& _assignment) {
		set <Token> compoundAssignment = { Token::AssignShr };
		if (compoundAssignment.count(_assignment.assignmentOperator()) > 0)
			cast_error(_assignment, "Unsupported operation.");
		if (tryAssignArrayLength(_assignment)) return;
		if (tryAssignMapping(_assignment)) return;
		if (tryAssignTuple(_assignment)) return;
		cast_error(_assignment, "Unsupported assignment.");
	}

};

}	// solidity
}	// dev