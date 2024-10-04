#include "alert.h"
#include "Error.h"
#include "Sema/Sema.h"

#define foreach(_Name, _Content) for (auto&& _Name : _Content)

#define printkind alertmsg(static_cast<int>(ast->kind))
#define printkind_of(_A) alertmsg(static_cast<int>((_A)->kind))

namespace fire::semantics_checker {

void Sema::check_full() {
  this->check(this->root);

  /*

  this->SaveScopeLocation();

  for (auto&& req : this->ins_requests) {

    this->_location = req.scope_loc;

    try {

      assert(req.cloned != nullptr);

      this->check(req.cloned);
    }
    catch (Error err) {
      string func_name = req.original->GetName() + "@<";

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

      throw err.AddChain(Error(Error::ER_Note, req.requested, "requested here"))
          .InLocation("in instantiation of '" + func_name + "'");
    }
  }

  this->RestoreScopeLocation();
  */
}

void Sema::check(ASTPointer ast) {

  if (!ast)
    return;

  switch (ast->kind) {

  case ASTKind::Class: {

    std::map<string, bool> bb;

    auto x = ASTCast<AST::Class>(ast);

    for (auto&& mv : x->member_variables) {
      if (bb[mv->GetName()])
        throw Error(mv->name, "duplicate member variable name");

      bb[mv->GetName()] = true;

      this->check(mv);
    }

    for (auto&& mf : x->member_functions) {
      this->check(mf);
    }

    break;
  }

  case ASTKind::Function: {
    auto x = ASTCast<AST::Function>(ast);

    // auto func = this->get_func(x);
    // SemaFunction* func = nullptr;
    FunctionScope* func = nullptr;

    for (auto&& [_Key, _Val] : this->function_scope_map) {
      if (_Key == x) {
        func = _Val;
        break;
      }
    }

    assert(func);

    if (func->is_templated()) {
      break;
    }

    auto pfunc = this->cur_function;
    this->cur_function = func;

    for (auto&& arg : func->arguments) {
      arg.deducted_type = this->eval_type(arg.arg->type);
      arg.is_type_deducted = true;
    }

    func->result_type = this->eval_type(x->return_type);

    this->EnterScope(x);

    AST::walk_ast(func->block->ast, [&func](AST::ASTWalkerLocation loc, ASTPointer _ast) {
      if (loc == AST::AW_Begin && _ast->kind == ASTKind::Return) {
        func->return_stmt_list.emplace_back(ASTCast<AST::Statement>(_ast));
      }
    });

    if (!func->result_type.equals(TypeKind::None)) {
      if (func->return_stmt_list.empty()) {
        Error(func->ast->token, "function must return value of type '" +
                                    func->result_type.to_string() +
                                    "', but don't return "
                                    "anything.")
            .emit();

        throw Error(Error::ER_Note, func->ast->return_type, "specified here");
      }
      else if (auto block = func->block;
               (*block->ast->list.rbegin())->kind != ASTKind::Return) {
        throw Error(block->ast->endtok, "expected return-statement before this token");
      }
    }

    this->check(x->block);

    this->LeaveScope();

    this->cur_function = pfunc;

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
    auto& curScope = this->GetCurScope();

    ScopeContext::LocalVar& var = ((BlockScope*)curScope)->variables[x->index];

    if (x->type) {
      var.deducted_type = this->eval_type(x->type);
      var.is_type_deducted = true;
    }

    if (x->init) {
      auto type = this->eval_type(x->init);

      if (x->type && !var.deducted_type.equals(type)) {
        throw Error(x->init, "expected '" + var.deducted_type.to_string() +
                                 "' type expression, but found '" + type.to_string() +
                                 "'");
      }

      var.deducted_type = type;
      var.is_type_deducted = true;
    }

    break;
  }

  case ASTKind::If: {
    auto d = ast->as_stmt()->get_data<AST::Statement::If>();

    if (!this->eval_type(d.cond).equals(TypeKind::Bool)) {
      throw Error(d.cond, "expected boolean expression");
    }

    this->check(d.if_true);
    this->check(d.if_false);

    ast->as_stmt()->set_data(d);
    break;
  }

  case ASTKind::While: {
    auto d = ast->as_stmt()->get_data<AST::Statement::While>();

    this->check(d.cond);
    this->check(d.block);

    ast->as_stmt()->set_data(d);
    break;
  }

  case ASTKind::TryCatch: {
    auto d = ast->as_stmt()->get_data<AST::Statement::TryCatch>();

    this->check(d.tryblock);

    vector<std::pair<ASTPointer, TypeInfo>> temp;

    for (auto&& c : d.catchers) {
      auto type = this->eval_type(c.type);

      for (auto&& c2 : d.catchers) {
        if (type.equals(c2._type)) {
          throw Error(c.type, "duplicated exception type")
              .AddChain(Error(Error::ER_Note, c2.type, "first defined here"));
        }
      }

      c._type = type;

      auto& e = ((BlockScope*)this->GetScopeOf(c.catched))->variables[0];

      e.name = c.varname.str;
      e.deducted_type = this->eval_type(c.type);
      e.is_type_deducted = true;

      this->check(c.catched);
    }

    ast->as_stmt()->set_data(d);
    break;
  }

  case ASTKind::Return: {
    this->ExpectType(this->cur_function->result_type, ast->as_stmt()->get_expr());
    break;
  }

  case ASTKind::Throw: {
    this->check(ast->as_stmt()->get_expr());
    break;
  }

  case ASTKind::Break:
  case ASTKind::Continue:
    break;

  default:
    this->eval_type(ast);
  }
}
} // namespace fire::semantics_checker