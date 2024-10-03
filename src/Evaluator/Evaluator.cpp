#include "Builtin.h"
#include "Evaluator.h"
#include "Error.h"

#define CAST(T) auto x = ASTCast<AST::T>(ast)

namespace fire::eval {

ObjPtr<ObjNone> _None;

Evaluator::Evaluator() {
  _None = ObjNew<ObjNone>();
}

Evaluator::~Evaluator() {
}

Evaluator::VarStack& Evaluator::push_stack(int var_size) {

  auto& stack = this->var_stack.emplace_front();

  stack.var_list.reserve((size_t)var_size);

  return stack;
}

void Evaluator::pop_stack() {
  this->var_stack.pop_front();
}

Evaluator::VarStack& Evaluator::get_cur_stack() {
  return *this->var_stack.begin();
}

Evaluator::VarStack& Evaluator::get_stack(int distance) {
  auto it = this->var_stack.begin();

  for (int i = 0; i < distance; i++)
    it++;

  return *it;
}

ObjPointer& Evaluator::eval_as_left(ASTPointer ast) {

  switch (ast->kind) {
  case ASTKind::IndexRef: {
    auto ex = ast->as_expr();

    return this->eval_index_ref(this->eval_as_left(ex->lhs), this->evaluate(ex->rhs));
  }
  }

  assert(ast->kind == ASTKind::Variable);

  auto x = ast->GetID();

  return this->get_stack(x->depth /*distance*/).var_list[x->index];
}

ObjPointer& Evaluator::eval_index_ref(ObjPointer array, ObjPointer _index_obj) {
  assert(_index_obj->type.kind == TypeKind::Int);

  i64 index = _index_obj->As<ObjPrimitive>()->vi;

  switch (array->type.kind) {
  case TypeKind::Dict: {
    todo_impl;
  }
  }

  assert(array->type.kind == TypeKind::Vector);

  return array->As<ObjIterable>()->list[(size_t)index];
}

ObjPointer Evaluator::evaluate(ASTPointer ast) {
  using Kind = ASTKind;

  if (!ast) {
    return _None;
  }

  switch (ast->kind) {

  case Kind::Identifier:
  case Kind::ScopeResol:
  case Kind::MemberAccess:
    // この２つの Kind は、意味解析で変更されているはず。
    // ここまで来ている場合、バグです。
    debug(Error(ast, "??").emit());
    panic;

  case Kind::Function:
  case Kind::Class:
  case Kind::Enum:
    break;

  case Kind::Value: {
    return ast->as_value()->value;
  }

  case Kind::Variable: {
    auto x = ast->GetID();

    return this->get_stack(x->depth /*distance*/).var_list[x->index];
  }

  case Kind::Array: {
    CAST(Array);

    auto obj = ObjNew<ObjIterable>(TypeInfo(TypeKind::Vector, {x->elem_type}));

    for (auto&& e : x->elements)
      obj->Append(this->evaluate(e));

    return obj;
  }

  case Kind::IndexRef: {

    todo_impl;
    break;
  }

  case Kind::FuncName: {
    auto id = ast->GetID();
    auto obj = ObjNew<ObjCallable>(id->candidates[0]);

    obj->type.params = id->ft_args;
    obj->type.params.insert(obj->type.params.begin(), id->ft_ret);

    return obj;
  }

  case Kind::BuiltinFuncName: {
    auto id = ast->GetID();
    auto obj = ObjNew<ObjCallable>(id->candidates_builtin[0]);

    obj->type.params = id->ft_args;
    obj->type.params.insert(obj->type.params.begin(), id->ft_ret);

    return obj;
  }

  case Kind::MemberVariable: {
    auto ex = ast->as_expr();

    auto inst = PtrCast<ObjInstance>(this->evaluate(ex->lhs));

    auto id = ASTCast<AST::Identifier>(ex->rhs);

    return inst->get_mvar(id->index);
  }

  case Kind::MemberFunction: {
    return ObjNew<ObjCallable>(ast->GetID()->candidates[0]);
  }

  case Kind::BuiltinMemberVariable: {
    auto self = ast->as_expr()->lhs;
    auto id = ast->GetID();

    return id->blt_member_var->impl(self, this->evaluate(self));
  }

  case Kind::BuiltinMemberFunction: {
    auto expr = ast->as_expr();
    auto id = expr->GetID();

    auto callable = ObjNew<ObjCallable>(id->candidates_builtin[0]);

    callable->selfobj = this->evaluate(expr->lhs);
    callable->is_member_call = true;

    return callable;
  }

  case Kind::CallFunc: {
    CAST(CallFunc);

    ObjVector args;

    for (auto&& arg : x->args) {
      args.emplace_back(this->evaluate(arg));
    }

    auto _func = x->callee_ast;
    auto _builtin = x->callee_builtin;

    if (x->call_functor) {
      auto functor = PtrCast<ObjCallable>(this->evaluate(x->callee));

      if (functor->func)
        _func = functor->func;
      else
        _builtin = functor->builtin;
    }

    if (_builtin) {
      return _builtin->Call(x, std::move(args));
    }

    auto& stack = this->push_stack(x->args.size());

    this->call_stack.push_front(&stack);

    stack.var_list = std::move(args);

    this->evaluate(_func->block);

    auto result = stack.func_result;

    this->pop_stack();
    this->call_stack.pop_front();

    return result ? result : _None;
  }

  case Kind::CallFunc_Ctor: {
    CAST(CallFunc);

    auto inst = ObjNew<ObjInstance>(x->get_class_ptr());

    for (auto&& arg : x->args) {
      inst->add_member_var(this->evaluate(arg));
    }

    return inst;
  }

  case Kind::Return: {
    auto stmt = ast->as_stmt();

    auto& stack = *this->call_stack.begin();

    stack->func_result = this->evaluate(stmt->get_expr());

    for (auto&& s : this->var_stack) {
      s.returned = true;

      if (&s == stack)
        break;
    }

    break;
  }

  case Kind::Break:
    (*this->loops.begin())->breaked = true;
    break;

  case Kind::Continue:
    (*this->loops.begin())->continued = true;
    break;

  case Kind::Block: {
    CAST(Block);

    auto& stack = this->push_stack(x->stack_size);

    for (auto&& y : x->list) {
      this->evaluate(y);

      if (stack.returned)
        break;
    }

    this->pop_stack();

    break;
  }

  case Kind::If: {
    auto d = ast->as_stmt()->get_data<AST::Statement::If>();

    auto cond = this->evaluate(d.cond);

    if (cond->get_vb())
      this->evaluate(d.if_true);
    else
      this->evaluate(d.if_false);

    break;
  }

  case Kind::For: {
    auto d = ast->as_stmt()->get_data<AST::Statement::For>();

    auto& stack = this->push_stack(d.block->stack_size);

    this->loops.push_front(&stack);

    this->evaluate(d.init);

    while (!d.cond || this->evaluate(d.cond)->get_vb()) {
      for (auto&& x : d.block->list) {
        this->evaluate(x);

        if (stack.breaked)
          goto __loop_break;

        if (stack.continued)
          break;
      }

      this->evaluate(d.step);
    }

  __loop_break:
    this->pop_stack();
    this->loops.pop_front();

    break;
  }

  case Kind::Vardef: {
    CAST(VarDef);

    if (x->init)
      this->get_cur_stack().var_list[x->index] = this->evaluate(x->init);

    break;
  }

  case Kind::Assign: {
    auto x = ast->as_expr();

    return this->eval_as_left(x->lhs) = this->evaluate(x->rhs);
  }

  default:
    if (ast->is_expr)
      return this->eval_expr(ASTCast<AST::Expr>(ast));

    alertexpr(static_cast<int>(ast->kind));
    todo_impl;
  }

  return _None;
}

static inline ObjPtr<ObjPrimitive> new_int(i64 v) {
  return ObjNew<ObjPrimitive>(v);
}

static inline ObjPtr<ObjPrimitive> new_float(double v) {
  return ObjNew<ObjPrimitive>(v);
}

static inline ObjPtr<ObjPrimitive> new_bool(bool b) {
  return ObjNew<ObjPrimitive>(b);
}

static inline ObjPtr<ObjIterable> multiply_array(ObjPtr<ObjIterable> s, i64 n) {
  ObjPtr<ObjIterable> ret = PtrCast<ObjIterable>(s->Clone());

  while (--n) {
    ret->AppendList(s);
  }

  return ret;
}

static inline ObjPtr<ObjIterable> add_vec_wrap(ObjPtr<ObjIterable> v, ObjPointer e) {
  v = PtrCast<ObjIterable>(v->Clone());

  v->Append(e);

  return v;
}

ObjPointer Evaluator::eval_expr(ASTPtr<AST::Expr> ast) {
  using Kind = ASTKind;

  ObjPointer lhs = this->evaluate(ast->lhs);
  ObjPointer rhs = this->evaluate(ast->rhs);

  switch (ast->kind) {

  case Kind::Add: {

    if (lhs->is_vector() && rhs->is_int())
      return add_vec_wrap(PtrCast<ObjIterable>(lhs), rhs);

    if (rhs->is_vector() && lhs->is_int())
      return add_vec_wrap(PtrCast<ObjIterable>(rhs), lhs);

    switch (lhs->type.kind) {
    case TypeKind::Int:
      return new_int(lhs->get_vi() + rhs->get_vi());

    case TypeKind::Float:
      return new_float(lhs->get_vf() + rhs->get_vf());

    case TypeKind::String:
      lhs = lhs->Clone();
      lhs->As<ObjString>()->AppendList(PtrCast<ObjIterable>(rhs));
      return lhs;

    default:
      todo_impl;
    }

    break;
  }

  case Kind::Sub: {
    switch (lhs->type.kind) {
    case TypeKind::Int:
      return new_int(lhs->get_vi() - rhs->get_vi());

    case TypeKind::Float:
      return new_float(lhs->get_vf() - rhs->get_vf());
    }

    break;
  }

  case Kind::Mul: {
    if ((lhs->is_string() || lhs->is_vector()) && rhs->is_int())
      return multiply_array(PtrCast<ObjIterable>(lhs), rhs->get_vi());

    if ((rhs->is_string() || rhs->is_vector()) && lhs->is_int())
      return multiply_array(PtrCast<ObjIterable>(rhs), lhs->get_vi());

    switch (lhs->type.kind) {
    case TypeKind::Int:
      return new_int(lhs->get_vi() * rhs->get_vi());

    case TypeKind::Float:
      return new_float(lhs->get_vf() * rhs->get_vf());
    }

    break;
  }

  case Kind::Div: {
    switch (lhs->type.kind) {
    case TypeKind::Int: {
      auto vi = rhs->get_vi();

      if (vi == 0)
        goto _divided_by_zero;

      return new_int(lhs->get_vi() / vi);
    }

    case TypeKind::Float: {
      auto vf = rhs->get_vf();

      if (vf == 0)
        goto _divided_by_zero;

      return new_float(lhs->get_vf() / vf);
    }
    }

    break;
  }

  case Kind::Mod: {
    auto vi = rhs->get_vi();

    if (vi == 0)
      goto _divided_by_zero;

    return new_int(lhs->get_vi() % vi);
  }

  case Kind::LShift:
    return new_int(lhs->get_vi() << rhs->get_vi());

  case Kind::RShift:
    return new_int(lhs->get_vi() >> rhs->get_vi());

  case Kind::Bigger: {
    switch (lhs->type.kind) {
    case TypeKind::Int:
      return new_bool(lhs->get_vi() > rhs->get_vi());

    case TypeKind::Float:
      return new_bool(lhs->get_vf() > rhs->get_vf());

    case TypeKind::Char:
      return new_bool(lhs->get_vc() > rhs->get_vc());
    }

    break;
  }

  case Kind::BiggerOrEqual: {
    switch (lhs->type.kind) {
    case TypeKind::Int:
      return new_bool(lhs->get_vi() >= rhs->get_vi());

    case TypeKind::Float:
      return new_bool(lhs->get_vf() >= rhs->get_vf());

    case TypeKind::Char:
      return new_bool(lhs->get_vc() >= rhs->get_vc());
    }

    break;
  }

  case Kind::LogAND:

  default:
    not_implemented("not implemented operator: " << lhs->type.to_string() << " "
                                                 << ast->op.str << " "
                                                 << rhs->type.to_string());
  }

  return lhs;

_divided_by_zero:
  throw Error(ast->op, "divided by zero");
}

} // namespace fire::eval