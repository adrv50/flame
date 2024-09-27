#include "alert.h"
#include "Error.h"
#include "Sema/Sema.h"

#define foreach(_Name, _Content) for (auto&& _Name : _Content)

#define printkind alertmsg(static_cast<int>(ast->kind))
#define printkind_of(_A) alertmsg(static_cast<int>((_A)->kind))

namespace fire::semantics_checker {

void Sema::check_full() {
  alert;
  this->check(this->root);

  alert;
  this->SaveScopeLocation();

  alertexpr(this->ins_requests.size());

  alert;
  for (auto&& req : this->ins_requests) {
    alert;

    this->_location = req.scope_loc;

    alert;
    try {

      alert;
      alertexpr(req.cloned.get());

      assert(req.cloned != nullptr);
      this->check(req.cloned);

      alert;
    }
    catch (Error err) {
      alert;
      string func_name = req.idinfo.to_string() + "@<";

      for (int i = -1; auto&& [_name, _data] : req.param_types) {
        i++;

        func_name += _name + "=" + _data.type.to_string();

        if (i + 1 < req.param_types.size())
          func_name += ", ";
      }

      func_name += ">(" +
                   utils::join<TypeInfo>(", ", req.arg_types,
                                         [](TypeInfo t) -> string {
                                           return t.to_string();
                                         }) +
                   ")";

      throw err.InLocation("in instantiation of '" + func_name + "'");
    }
    alert;
  }

  alert;
  this->RestoreScopeLocation();

  alert;
}

void Sema::check(ASTPointer ast) {

  if (!ast) {
    return;
  }

  switch (ast->kind) {

  case ASTKind::Function: {
    auto x = ASTCast<AST::Function>(ast);

    // auto func = this->get_func(x);
    SemaFunction* func = nullptr;

    for (auto&& [_Key, _Val] : this->function_scope_map) {
      if (_Key == x) {
        func = &_Val;
        break;
      }
    }

    assert(func);

    if (func->func->is_templated) {
      break;
    }

    this->EnterScope(x);

    func->result_type = this->evaltype(x->return_type);

    AST::walk_ast(func->func->block,
                  [&func](AST::ASTWalkerLocation loc, ASTPointer _ast) {
                    if (loc == AST::AW_Begin && _ast->kind == ASTKind::Return) {
                      func->return_stmt_list.emplace_back(ASTCast<AST::Statement>(_ast));
                    }
                  });

    for (auto&& ret : func->return_stmt_list) {
      auto expr = ret->As<AST::Statement>()->get_expr();

      if (auto type = this->evaltype(expr); !type.equals(func->result_type)) {
        if (func->result_type.equals(TypeKind::None)) {

          //
          // TODO: Suggest return type specification
          //

          throw Error(ret->token, "expected ';' after this token");
        }
        else if (!expr) {
          Error(ret->token, "expected '" + func->result_type.to_string() +
                                "' type expression after this token")
              .emit();
        }
        else {
          Error(expr, "expected '" + func->result_type.to_string() +
                          "' type expression, but found '" + type.to_string() + "'")
              .emit();
        }

        goto _return_type_note;
      }
    }

    if (!func->result_type.equals(TypeKind::None)) {
      if (func->return_stmt_list.empty()) {
        Error(func->func->token, "function must return value of type '" +
                                     func->result_type.to_string() +
                                     "', but don't return "
                                     "anything.")
            .emit();

      _return_type_note:
        throw Error(func->func->return_type, "specified here");
      }
      else if (auto block = func->func->block;
               (*block->list.rbegin())->kind != ASTKind::Return) {
        throw Error(block->endtok, "expected return-statement before this token");
      }
    }

    this->check(x->block);

    this->LeaveScope();

    break;
  }

  case ASTKind::Block: {
    auto x = ASTCast<AST::Block>(ast);

    this->EnterScope(x);

    for (auto&& e : x->list)
      this->check(e);

    this->LeaveScope();

    break;
  }

  case ASTKind::Vardef: {

    auto x = ASTCast<AST::VarDef>(ast);

    this->check(x->type);
    this->check(x->init);

    break;
  }

  case ASTKind::Throw: {
    auto x = ASTCast<AST::Statement>(ast);

    this->check(x->get_expr());

    break;
  }

  case ASTKind::Return: {
    auto x = ASTCast<AST::Statement>(ast);

    this->check(x->get_expr());

    for (auto s = this->_location.History.rbegin(); s != this->_location.History.rend();
         s++) {
      if ((*s)->type == ScopeContext::SC_Func) {
        break;
      }

      x->ret_func_scope_distance++;
    }

    break;
  }

  default:
    this->evaltype(ast);
  }
}
} // namespace fire::semantics_checker