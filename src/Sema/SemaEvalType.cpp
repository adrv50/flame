#include "alert.h"
#include "Sema/Sema.h"
#include "Error.h"
#include "Builtin.h"

#define foreach(_Name, _Content) for (auto&& _Name : _Content)

#define printkind alertmsg(static_cast<int>(ast->kind))
#define printkind_of(_A) alertmsg(static_cast<int>((_A)->kind))

namespace fire::semantics_checker {

TypeInfo Sema::eval_type(ASTPointer ast) {
  using Kind = ASTKind;

  if (!ast)
    return {};

  switch (ast->kind) {
  case Kind::Enum:
  case Kind::Function:
    return {};

  case Kind::TypeName: {
    // clang-format off
    static std::map<TypeKind, char const*> kind_name_map {
      { TypeKind::None,       "none" },
      { TypeKind::Int,        "int" },
      { TypeKind::Float,      "float" },
      { TypeKind::Bool,       "bool" },
      { TypeKind::Char,       "char" },
      { TypeKind::String,     "string" },
      { TypeKind::Vector,     "vector" },
      { TypeKind::Tuple,      "tuple" },
      { TypeKind::Dict,       "dict" },
      { TypeKind::Instance,   "instance" },
      { TypeKind::Module,     "module" },
      { TypeKind::Function,   "function" },
      { TypeKind::Module,     "module" },
      { TypeKind::TypeName,   "type" },
    };
    // clang-format on

    auto x = ASTCast<AST::TypeName>(ast);

    TypeInfo type;

    for (auto&& [key, val] : kind_name_map) {
      if (val == x->GetName()) {
        type = key;

        if (int c = type.needed_param_count();
            c == 0 && (int)x->type_params.size() >= 1) {
          throw Error(x->token, "type '" + string(val) + "' cannot have parameters");
        }
        else if (c >= 1) {
          if ((int)x->type_params.size() < c)
            throw Error(x->token, "too few parameters");
          else if ((int)x->type_params.size() > c)
            throw Error(x->token, "too many parameters");
        }

        for (auto&& param : x->type_params) {
          type.params.emplace_back(this->eval_type(param));
        }

        return type;
      }
    }

    for (auto&& inst : this->inst_scope) {
      if (auto p = inst.find_name(x->GetName()); p) {
        return *p;
      }
    }

    auto rs = this->find_name(x->GetName());

    switch (rs.type) {
    case NameType::Enum:
      return TypeInfo::from_enum(rs.ast_enum);

    case NameType::Class:
      return TypeInfo::from_class(rs.ast_class);
    }

    // type.name = x->GetName();
    // type.kind = TypeKind::Unknown;
    // return type;

    throw Error(x->token, "unknown type name");
  }

  case Kind::Value: {
    return ast->as_value()->value->type;
  }

  case Kind::Variable: {
    auto pvar = this->_find_variable(ast->GetID()->GetName());

    assert(pvar->is_type_deducted);

    return pvar->deducted_type;
  }

  case Kind::FuncName: {
    auto id = ast->GetID();

    TypeInfo type = TypeKind::Function;

    type.params = id->ft_args;
    type.params.insert(type.params.begin(), id->ft_ret);

    return type;
  }

  case Kind::Array: {
    auto x = ast->As<AST::Array>();
    TypeInfo type = TypeKind::Vector;

    if (x->elements.empty()) {
      if (this->IsExpected(TypeKind::Vector)) {
        return x->elem_type = *this->GetExpectedType();
      }

      throw Error(x->token, "cannot deduction element type")
          .AddNote("you can use 'array' keyword for specify type. (like "
                   "\"array<T>([]...])\")");
    }

    auto const& elemType = type.params.emplace_back(this->eval_type(x->elements[0]));

    x->elem_type = elemType;

    for (auto it = x->elements.begin() + 1; it != x->elements.end(); it++) {
      if (!elemType.equals(this->eval_type(*it))) {
        throw Error(*it, "expected '" + elemType.to_string() +
                             "' type expression as element in array");
      }
    }

    return type;
  }

  case Kind::OverloadResolutionGuide: {
    auto x = ASTCast<AST::Expr>(ast);

    x->lhs->GetID()->must_completed = false;

    auto functor_ast = x->lhs;
    auto functor = this->eval_type(functor_ast);

    if (functor_ast->kind != Kind::FuncName) {
      throw Error(functor_ast, "expected function name");
    }

    auto id = functor_ast->GetID();

    if (id->candidates.size() == 1) {
      throw Error(x->op, "use operator 'of' to non ambiguous function name is not valid");
    }

    alert;

    auto sig_ast = ASTCast<AST::Signature>(x->rhs);
    auto sig = this->eval_type(sig_ast);

    auto const& sig_ret = sig.params[0];
    auto const sig_args = TypeVec(sig.params.begin() + 1, sig.params.end());

    struct _Temp {
      ASTPtr<AST::Function> func;
      TypeVec template_params;
      TypeVec arg_types;
      TypeInfo ret;
    };

    vector<_Temp> final_cd;

    for (auto&& cd : id->candidates) {
      alert;

      if ((sig_ast->arg_type_list.size() < cd->arguments.size()) ||
          (!cd->is_var_arg && cd->arguments.size() < sig_ast->arg_type_list.size()))
        continue;

      if (cd->template_param_names.size() < id->template_args.size())
        continue;

      if (cd->is_templated) {
        struct _holder {
          string name;
          TypeInfo type;
          bool set = false;
        };

        vector<_holder> params;

        for (auto&& tp : cd->template_param_names) {
          params.emplace_back(_holder{tp.str, {}, false});
        }

        for (size_t i = 0; i < id->template_args.size(); i++) {
          params[i].type = id->template_args[i];
          params[i].set = true;
        }

        // compare template-args and arg-types
        for (size_t i = 0; i < cd->arguments.size(); i++) {
          auto& act = sig.params[i + 1];

          for (auto&& p : params) {
            if (p.name == cd->arguments[i]->type->GetName()) {
              if (p.set && !p.type.equals(act)) {
                alert;
                goto _pass_candidate;
              }

              p.type = act;
              p.set = true;

              break;
            }
          }
        }

        for (auto&& p : params) {
          if (!p.set) {
            alert;
            goto _pass_candidate;
          }
        }

        auto& inst = this->enter_instantiation_scope();

        for (auto&& p : params)
          inst.add_name(p.name, p.type);

        auto cd_sig = _Temp{cd, {}, {}, this->eval_type(cd->return_type)};

        for (auto&& arg : cd->arguments) {
          cd_sig.arg_types.emplace_back(this->eval_type(arg->type));
        }

        this->leave_instantiation_scope();

        for (auto&& arg : params) {
          cd_sig.template_params.emplace_back(arg.type);
        }

        for (size_t i = 0; i < cd->arguments.size(); i++) {
          if (!sig_args[i].equals(cd_sig.arg_types[i])) {
            alert;
            goto _pass_candidate;
          }
        }

        if (!cd_sig.ret.equals(sig_ret)) {
          alert;
          goto _pass_candidate;
        }

        final_cd.emplace_back(std::move(cd_sig));
      }
      else {
        for (size_t i = 0; i < sig_args.size(); i++) {
          if (!sig_args[i].equals(this->eval_type(cd->arguments[i]->type))) {
            alert;
            goto _pass_candidate;
          }
        }

        if (auto _ret = this->eval_type(cd->return_type); sig_ret.equals(_ret))
          final_cd.emplace_back(_Temp{cd, {}, sig_args, _ret});
      }
    _pass_candidate:;
    }

    if (final_cd.empty()) {

      throw Error(x->op, "cannot find function '" + id->GetName() +
                             "' matching to signature '(" +
                             utils::join<TypeInfo>(", ", sig_args,
                                                   [](auto t) {
                                                     return t.to_string();
                                                   }) +
                             ") -> " + sig_ret.to_string() + "'");
    }
    else if (final_cd.size() >= 2) {
      Error e{x->op, "function name '" + id->GetName() + "' is still ambiguous"};

      for (auto&& cd : final_cd)
        e.AddChain(Error(Error::ER_Note, cd.func, "candidate:"));

      throw e;
    }

    TypeInfo ret = TypeKind::Function;

    ret.params = final_cd[0].arg_types;
    ret.params.insert(ret.params.begin(), final_cd[0].ret);

    return ret;
  }

  case Kind::Signature: {
    TypeInfo ret = TypeKind::Function;

    auto sig = ASTCast<AST::Signature>(ast);

    ret.params.emplace_back(this->eval_type(sig->result_type));

    for (auto&& t : sig->arg_type_list)
      ret.params.emplace_back(this->eval_type(t));

    return ret;
  }

  case Kind::Identifier: {

    auto id = ASTCast<AST::Identifier>(ast);
    auto idinfo = this->get_identifier_info(id);

    auto& res = idinfo.result;

    //
    // TypeKind::Function:
    //
    // type.params =
    //   { return-type, arg1, arg2, ...(arg) }
    //

    switch (res.type) {
    case NameType::Var: {
      id->kind = ASTKind::Variable;

      if (id->id_params.size() >= 1)
        throw Error(id->paramtok, "invalid use type argument for variable");

      if (!res.lvar->is_type_deducted)
        throw Error(ast->token, "cannot use variable before assignment");

      // distance
      id->depth = this->GetCurScope()->depth - res.lvar->depth;

      id->index = res.lvar->index;

      return res.lvar->deducted_type;
    }

    case NameType::Func: {
      id->kind = ASTKind::FuncName;

      id->candidates = std::move(idinfo.result.functions);

      ASTVec<AST::Function> temp;

      bool is_all_template = true;

      for (auto&& c : id->candidates) {
        if (!c->is_templated)
          is_all_template = false;

        if (id->must_completed) {
          if (c->template_param_names.size() != id->id_params.size()) {
            continue;
          }
        }
        else if (c->template_param_names.size() < id->id_params.size()) {
          continue;
        }

        temp.emplace_back(c);
      }

      if (temp.empty() && is_all_template) {
        throw Error(id->token, "cannot use function name '" + id->GetName() +
                                   "' without template arguments");
      }

      id->candidates = std::move(temp);

      if (id->candidates.size() >= 2) {
        if (!id->must_completed)
          return TypeKind::Function;

        auto err =
            Error(id->token, "function name '" + idinfo.to_string() + "' is ambigous.");

        err.AddChain(Error(Error::ER_Note, id->candidates[0], "candidate 0:"))
            .AddChain(Error(Error::ER_Note, id->candidates[1], "candidate 1:"));

        if (id->candidates.size() >= 3) {
          err.AddNote(
              utils::Format("and found %zu candidates ...", id->candidates.size() - 2));
        }

        throw err;
      }

      assert(id->candidates.size() == 1);

      TypeInfo type = TypeKind::Function;

      ASTPtr<AST::Function> func = id->candidates[0];

      type.is_free_args = func->is_var_arg;

      if (func->is_templated) {
        auto& inst = this->enter_instantiation_scope();

        for (auto it = id->template_args.begin();
             auto&& param : func->template_param_names) {

          if (it == id->template_args.end()) {
            if (id->must_completed) {
              throw Error(id, "type of template argument '" + param.str + "' is missing");
            }

            this->leave_instantiation_scope();
            return TypeKind::Function;
          }

          inst.add_name(param.str, *it);
        }

        this->SaveScopeLocation();

        auto scope = (FunctionScope*)this->GetScopeOf(func);

        this->BackToDepth(scope->depth - 1);

        this->EnterScope(scope);

        for (size_t i = 0; auto&& t : idinfo.id_params) {
          auto& arg = scope->arguments[i];

          arg.deducted_type = t;
          arg.is_argument = true;
          arg.is_type_deducted = true;

          i++;
        }

        this->check(func->block);

        this->RestoreScopeLocation();
      }

      // if (!func->is_templated) {
      type.params.emplace_back(id->ft_ret = this->eval_type(func->return_type));

      for (auto&& arg : func->arguments) {
        id->ft_args.emplace_back(type.params.emplace_back(this->eval_type(arg->type)));
      }
      // }

      if (func->is_templated) {
        this->leave_instantiation_scope();
      }

      return type;
    }

    case NameType::BuiltinFunc: {
      id->kind = ASTKind::BuiltinFuncName;

      id->candidates_builtin = std::move(idinfo.result.builtin_funcs);

      if (id->candidates_builtin.size() >= 2) {
        if (!id->sema_allow_ambigious)
          throw Error(id->token, "function name '" + id->GetName() + "' is ambigous.");

        return TypeKind::Function; // ambigious -> called by case CallFunc
      }

      assert(id->candidates_builtin.size() == 1);

      TypeInfo type = TypeKind::Function;

      builtins::Function const* func = id->candidates_builtin[0];

      type.is_free_args = func->is_variable_args;

      type.params = id->ft_args = func->arg_types;
      type.params.insert(type.params.begin(), id->ft_ret = func->result_type);

      return type;
    }

    case NameType::Class: {
      id->kind = ASTKind::ClassName;
      id->ast_class = idinfo.result.ast_class;

      return TypeInfo::from_class(id->ast_class);
    }

    case NameType::Enum: {
      id->kind = ASTKind::EnumName;
      id->ast_enum = idinfo.result.ast_enum;

      return TypeInfo::from_enum(id->ast_enum);
    }

    case NameType::TypeName: {
      auto type = TypeInfo(idinfo.result.kind, idinfo.id_params);

      if (size_t c = type.needed_param_count(); c == 0 && type.params.size() >= 1) {
        throw Error(id, "'" + id->GetName() + "' is not template type");
      }
      else if (c != type.params.size()) {
        throw Error(id, "no match template argument count");
      }

      return TypeInfo(TypeKind::TypeName, {type});
    }

    case NameType::Unknown:
      throw Error(ast->token, "cannot find name '" + id->GetName() + "'");

    default:
      todo_impl;
    }

    break;
  }

  case Kind::ScopeResol: {
    auto scoperesol = ASTCast<AST::ScopeResol>(ast);
    auto idinfo = this->get_identifier_info(scoperesol);

    switch (idinfo.result.type) {
    case NameType::Var: {
      todo_impl;
      ast->kind = ASTKind::Variable;
      break;
    }

    case NameType::Enumerator: {
      ast->kind = Kind::Enumerator;

      auto id = ast->GetID();

      id->ast_enum = idinfo.result.ast_enum;
      id->index = idinfo.result.enumerator_index;

      TypeInfo type = TypeKind::Enumerator;

      type.name = id->ast_enum->GetName();
      type.enum_index = id->index;

      return type;
    }

    case NameType::MemberFunc: {
      auto id = ast->GetID();

      ast->kind = ASTKind::FuncName;

      id->candidates = idinfo.result.functions;

      if (!id->sema_allow_ambigious) {
        if (id->candidates.size() >= 2) {
          throw Error(id->token, "function name '" + id->GetName() + "' is ambigous.");
        }

        TypeInfo type{TypeKind::Function};

        alert;
        type.params = {this->eval_type(id->candidates[0]->return_type)};

        for (auto&& arg : id->candidates[0]->arguments) {
          alert;
          type.params.emplace_back(this->eval_type(arg->type));
        }

        alert;
        return type;
      }

      return TypeKind::Function;
    }

    case NameType::Unknown:
      todo_impl;
      break;
    }

    todo_impl;

    break;
  }

  case Kind::Enumerator: {
    TypeInfo type = TypeKind::Enumerator;

    auto id = ast->GetID();

    type.type_ast = id->ast_enum;
    type.name = id->ast_enum->GetName();
    type.enum_index = id->index;

    return type;
  }

  case Kind::EnumName:
    return TypeInfo::from_enum(ast->GetID()->ast_enum);

  case Kind::ClassName:
    return TypeInfo::from_class(ast->GetID()->ast_class);

  case Kind::CallFunc: {
    auto call = ASTCast<AST::CallFunc>(ast);

    ASTPointer functor = call->callee;

    ASTPtr<AST::Identifier> id = nullptr; // -> functor (if id or scoperesol)

    if (functor->is_ident_or_scoperesol() || functor->kind == ASTKind::MemberAccess) {
      id = Sema::GetID(functor);

      id->sema_allow_ambigious = true;
      id->sema_guess_parameter = true;

      id->must_completed = false;
    }

    TypeInfo functor_type = this->eval_type(functor);

    TypeVec arg_types;

    if (functor->kind == ASTKind::MemberFunction) {
      call->args.insert(call->args.begin(), functor->as_expr()->lhs);
      functor->kind = ASTKind::FuncName;
    }

    for (ASTPointer arg : call->args) {
      arg_types.emplace_back(this->eval_type(arg));
    }

    switch (functor->kind) {
      //
      // ユーザー定義の関数名:
      //
    case ASTKind::FuncName: {

      ASTVec<AST::Function> final_candidates;

      for (ASTPtr<AST::Function> candidate : id->candidates) {
        if (id->id_params.size() > candidate->template_param_names.size())
          continue;
        if (candidate->is_templated) {
          auto instantiated = this->instantiate_template_func(
              candidate, call, id, call->args, arg_types, false);

          if (instantiated != nullptr)
            final_candidates.emplace_back(instantiated);
        }
        else {
          TypeVec formal_arg_types;

          for (ASTPtr<AST::Argument> arg : candidate->arguments)
            formal_arg_types.emplace_back(this->eval_type(arg->type));

          auto res = this->check_function_call_parameters(
              call->args, candidate->is_var_arg, formal_arg_types, arg_types, false);

          if (res.result == ArgumentCheckResult::Ok) {
            final_candidates.emplace_back(candidate);
          }
        }
      }

      if (final_candidates.empty()) {
        throw Error(functor, "a function '" + AST::ToString(id) + "(" +
                                 utils::join<TypeInfo>(", ", arg_types,
                                                       [](TypeInfo t) {
                                                         return t.to_string();
                                                       }) +
                                 ")" + "' is not defined");
      }

      else if (final_candidates.size() >= 2) {
        throw Error(functor, "function name '" + AST::ToString(id) + "' is ambigious");
      }

      call->callee_ast = final_candidates[0];

      return this->eval_type(final_candidates[0]->return_type);
    }

    case ASTKind::BuiltinMemberFunction:
    case ASTKind::BuiltinFuncName: {

      for (builtins::Function const* fn : id->candidates_builtin) {
        auto res = this->check_function_call_parameters(call->args, fn->is_variable_args,
                                                        fn->arg_types, arg_types, false);

        if (res.result == ArgumentCheckResult::Ok) {
          call->callee_builtin = fn;

          if (functor->kind == ASTKind::BuiltinMemberFunction)
            call->args.insert(call->args.begin(), functor->as_expr()->lhs);

          return fn->result_type;
        }
      }

      break;
    }

      //
      // class-name:
      //   --> create instance
      //
    case ASTKind::ClassName: {
      auto const& xmv = id->ast_class->member_variables;

      if (arg_types.size() < xmv.size()) {
        throw Error(functor, "too few constructor arguments");
      }
      else if (arg_types.size() > xmv.size()) {
        throw Error(functor, "too many constructor arguments");
      }

      for (i64 i = 0; i < (i64)xmv.size(); i++) {
        if (auto ttt = this->eval_type(xmv[i]->type ? xmv[i]->type : xmv[i]->init);
            !arg_types[i].equals(ttt)) {
          throw Error(call->args[i], "expected '" + ttt.to_string() +
                                         "' type expression, but found '" +
                                         arg_types[i].to_string() + "'");
        }
      }

      ast->kind = ASTKind::CallFunc_Ctor;

      return TypeInfo::from_class(id->ast_class);
    }

    default: {
      if (functor_type.kind != TypeKind::Function) {
        break;
      }

      if (functor_type.params.empty()) {
        panic;
      }

      TypeVec formal = functor_type.params;

      TypeInfo ret = formal[0];

      formal.erase(formal.begin());

      if (functor_type.is_member_func) {
        formal.erase(formal.begin());
      }

      auto res = this->check_function_call_parameters(
          call->args, functor_type.is_free_args, formal, arg_types, false);

      if (res.result != ArgumentCheckResult::Ok) {
        throw Error(call->token, "arguments are not matching to signature '" +
                                     functor_type.to_string() + "'");
      }

      call->call_functor = true;

      return ret;
    }
    }

    throw Error(functor,
                "expected calllable expression or name of function or class or enum");
  }

  case Kind::CallFunc_Ctor: {
    return TypeInfo::from_class(ast->As<CallFunc>()->get_class_ptr());
  }

  case Kind::SpecifyArgumentName:
    return this->eval_type(ast->as_expr()->rhs);

  case Kind::IndexRef: {
    auto x = ASTCast<AST::Expr>(ast);

    auto arr = this->eval_type(x->lhs);

    switch (arr.kind) {
    case TypeKind::Vector:
      return arr.params[0];
    }

    throw Error(x->op, "'" + arr.to_string() + "' type is not subscriptable");
  }

  case Kind::MemberAccess: {

    ASTPtr<AST::Expr> expr = ASTCast<AST::Expr>(ast);

    TypeInfo left_type = this->eval_type(expr->lhs);

    ASTPtr<AST::Identifier> rhs_id = Sema::GetID(expr);

    string name = rhs_id->GetName();

    alertexpr(rhs_id->sema_allow_ambigious);

    switch (left_type.kind) {
    case TypeKind::Instance: {
      switch (left_type.type_ast->kind) {
      case ASTKind::Class: {
        auto x = ASTCast<AST::Class>(left_type.type_ast);

        alertexpr(x);

        for (i64 index = 0; auto&& mv : x->member_variables) {
          if (name == mv->GetName()) {
            expr->kind = ASTKind::MemberVariable;
            rhs_id->index = index;
            rhs_id->ast_class = x;

            return this->eval_type(mv->type);
          }

          index++;
        }

        for (auto&& mf : x->member_functions) {
          if (mf->member_of && mf->GetName() == name) {
            rhs_id->candidates.emplace_back(mf);
          }
        }

        break;
      }
      }
      break;
    }

    case TypeKind::Enumerator: {
      //
      // TODO:
      //   列挙型で、構造体のメンバ定義されてる場合、そのメンバを探す
      //
      // enum Kind {
      //   A(a: int, b: string)
      // }
      //
      //   ^ 左辺の値が Kind::A のとき、右辺が a であればその値を返す
      // (Evaluator での実装も必要)
      //
      todo_impl;
    }
    }

    if (!rhs_id->sema_allow_ambigious && rhs_id->candidates.size() >= 2) {
      goto _ambiguous_err;
    }

    if (!rhs_id->candidates.empty()) {
      expr->kind = ASTKind::MemberFunction;
      rhs_id->self_type = left_type;

      return this->eval_type(rhs_id->candidates[0]->return_type);
    }

    for (auto const& [self_type, func] : builtins::get_builtin_member_functions()) {
      if (left_type.equals(self_type) && func.name == rhs_id->GetName()) {
        rhs_id->candidates_builtin.emplace_back(&func);
      }
    }

    if (!rhs_id->sema_allow_ambigious && rhs_id->candidates_builtin.size() >= 2) {
    _ambiguous_err:
      throw Error(rhs_id, "member function '" + left_type.to_string() + "::" + name +
                              "' is ambiguous");
    }

    for (builtins::MemberVariable const& mvar :
         builtins::get_builtin_member_variables()) {
      if (left_type.equals(mvar.self_type) && mvar.name == name) {
        expr->kind = ASTKind::BuiltinMemberVariable;
        rhs_id->blt_member_var = &mvar;
        return mvar.result_type;
      }
    }

    if (rhs_id->candidates_builtin.size() >= 1) {
      expr->kind = ASTKind::BuiltinMemberFunction;
      rhs_id->self_type = left_type;

      return rhs_id->candidates_builtin.size() == 1
                 ? this->make_functor_type(rhs_id->candidates_builtin[0])
                 : TypeKind::Function;
    }

    throw Error(rhs_id, "type '" + left_type.to_string() + "' don't have a member '" +
                            name + "'");
  }

  case Kind::MemberVariable: {
    return this->eval_type(
        ast->GetID()->ast_class->member_variables[ast->GetID()->index]->type);
  }

  case Kind::MemberFunction: {
    todo_impl;
  }

  case Kind::BuiltinMemberVariable:
    todo_impl;

  case Kind::BuiltinMemberFunction:
    todo_impl;

  case Kind::Not: {
    auto x = ASTCast<AST::Expr>(ast);

    auto type = this->eval_type(x->lhs);

    if (type.kind != TypeKind::Bool)
      throw Error(x->lhs, "expected boolean expression");

    return type;
  }

  case Kind::Assign: {
    auto x = ASTCast<AST::Expr>(ast);

    auto src = this->eval_type(x->rhs);

    if (x->lhs->kind == ASTKind::Identifier || x->lhs->kind == ASTKind::ScopeResol) {
      auto idinfo = this->get_identifier_info(x->lhs);

      if (auto lvar = idinfo.result.lvar; idinfo.result.type == NameType::Var) {

        lvar->deducted_type = src;
        lvar->is_type_deducted = true;
      }
    }

    auto dest = this->eval_type(x->lhs);

    if (!dest.equals(src)) {
      throw Error(x->rhs, "expected '" + dest.to_string() + "' type expression");
    }

    if (!this->IsWritable(x->lhs)) {
      throw Error(x->lhs, "expected writable expression");
    }

    return dest;
  }

  default:
    if (ast->is_expr) {
      return this->EvalExpr(ASTCast<AST::Expr>(ast));
    }

    Error(ast, "").emit();
    alertmsg(static_cast<int>(ast->kind));
    todo_impl;
  }

  return TypeKind::None;
}

} // namespace fire::semantics_checker