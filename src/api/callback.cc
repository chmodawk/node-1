#include "node.h"
#include "async_wrap-inl.h"
#include "env-inl.h"
#include "v8.h"

namespace node {

using v8::Context;
using v8::EscapableHandleScope;
using v8::Function;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::MicrotasksScope;
using v8::NewStringType;
using v8::Object;
using v8::String;
using v8::Value;

CallbackScope::CallbackScope(Isolate* isolate,
                             Local<Object> object,
                             async_context asyncContext)
  : private_(new InternalCallbackScope(Environment::GetCurrent(isolate),
                                       object,
                                       asyncContext)),
    try_catch_(isolate) {
  try_catch_.SetVerbose(true);
}

CallbackScope::~CallbackScope() {
  if (try_catch_.HasCaught())
    private_->MarkAsFailed();
  delete private_;
}

InternalCallbackScope::InternalCallbackScope(AsyncWrap* async_wrap, int flags)
    : InternalCallbackScope(async_wrap->env(),
                            async_wrap->object(),
                            { async_wrap->get_async_id(),
                              async_wrap->get_trigger_async_id() },
                            flags) {}

InternalCallbackScope::InternalCallbackScope(Environment* env,
                                             Local<Object> object,
                                             const async_context& asyncContext,
                                             int flags)
  : env_(env),
    async_context_(asyncContext),
    object_(object),
    skip_hooks_(flags & kSkipAsyncHooks),
    skip_task_queues_(flags & kSkipTaskQueues) {
  CHECK_NOT_NULL(env);
  env->PushAsyncCallbackScope();

  if (!env->can_call_into_js()) {
    failed_ = true;
    return;
  }

  HandleScope handle_scope(env->isolate());
  // If you hit this assertion, you forgot to enter the v8::Context first.
  CHECK_EQ(Environment::GetCurrent(env->isolate()), env);

  env->async_hooks()->push_async_context(
    async_context_.async_id, async_context_.trigger_async_id, object);

  pushed_ids_ = true;

  if (asyncContext.async_id != 0 && !skip_hooks_) {
    // No need to check a return value because the application will exit if
    // an exception occurs.
    AsyncWrap::EmitBefore(env, asyncContext.async_id);
  }
}

InternalCallbackScope::~InternalCallbackScope() {
  Close();
  env_->PopAsyncCallbackScope();
}

void InternalCallbackScope::Close() {
  if (closed_) return;
  closed_ = true;

  if (!env_->can_call_into_js()) return;
  auto perform_stopping_check = [&]() {
    if (env_->is_stopping()) {
      MarkAsFailed();
      env_->async_hooks()->clear_async_id_stack();
    }
  };
  perform_stopping_check();

  if (!failed_ && async_context_.async_id != 0 && !skip_hooks_) {
    AsyncWrap::EmitAfter(env_, async_context_.async_id);
  }

  if (pushed_ids_)
    env_->async_hooks()->pop_async_context(async_context_.async_id);

  if (failed_) return;

  if (env_->async_callback_scope_depth() > 1 || skip_task_queues_) {
    return;
  }

  TickInfo* tick_info = env_->tick_info();

  if (!env_->can_call_into_js()) return;

  auto weakref_cleanup = OnScopeLeave([&]() { env_->RunWeakRefCleanup(); });

  if (!tick_info->has_tick_scheduled()) {
    MicrotasksScope::PerformCheckpoint(env_->isolate());

    perform_stopping_check();
  }

  // Make sure the stack unwound properly. If there are nested MakeCallback's
  // then it should return early and not reach this code.
  if (env_->async_hooks()->fields()[AsyncHooks::kTotals]) {
    CHECK_EQ(env_->execution_async_id(), 0);
    CHECK_EQ(env_->trigger_async_id(), 0);
  }

  if (!tick_info->has_tick_scheduled() && !tick_info->has_rejection_to_warn()) {
    return;
  }

  HandleScope handle_scope(env_->isolate());
  Local<Object> process = env_->process_object();

  if (!env_->can_call_into_js()) return;

  Local<Function> tick_callback = env_->tick_callback_function();

  // The tick is triggered before JS land calls SetTickCallback
  // to initializes the tick callback during bootstrap.
  CHECK(!tick_callback.IsEmpty());

  if (tick_callback->Call(env_->context(), process, 0, nullptr).IsEmpty()) {
    failed_ = true;
  }
  perform_stopping_check();
}

MaybeLocal<Value> InternalMakeCallback(Environment* env,
                                       Local<Object> resource,
                                       Local<Object> recv,
                                       const Local<Function> callback,
                                       int argc,
                                       Local<Value> argv[],
                                       async_context asyncContext) {
  CHECK(!recv.IsEmpty());
#ifdef DEBUG
  for (int i = 0; i < argc; i++)
    CHECK(!argv[i].IsEmpty());
#endif

  Local<Function> hook_cb = env->async_hooks_callback_trampoline();
  int flags = InternalCallbackScope::kNoFlags;
  bool use_async_hooks_trampoline = false;
  AsyncHooks* async_hooks = env->async_hooks();
  if (!hook_cb.IsEmpty()) {
    // Use the callback trampoline if there are any before or after hooks, or
    // we can expect some kind of usage of async_hooks.executionAsyncResource().
    flags = InternalCallbackScope::kSkipAsyncHooks;
    use_async_hooks_trampoline =
        async_hooks->fields()[AsyncHooks::kBefore] +
        async_hooks->fields()[AsyncHooks::kAfter] +
        async_hooks->fields()[AsyncHooks::kUsesExecutionAsyncResource] > 0;
  }

  InternalCallbackScope scope(env, resource, asyncContext, flags);
  if (scope.Failed()) {
    return MaybeLocal<Value>();
  }

  MaybeLocal<Value> ret;

  if (use_async_hooks_trampoline) {
    MaybeStackBuffer<Local<Value>, 16> args(3 + argc);
    args[0] = v8::Number::New(env->isolate(), asyncContext.async_id);
    args[1] = resource;
    args[2] = callback;
    for (int i = 0; i < argc; i++) {
      args[i + 3] = argv[i];
    }
    ret = hook_cb->Call(env->context(), recv, args.length(), &args[0]);
  } else {
    ret = callback->Call(env->context(), recv, argc, argv);
  }

  if (ret.IsEmpty()) {
    scope.MarkAsFailed();
    return MaybeLocal<Value>();
  }

  scope.Close();
  if (scope.Failed()) {
    return MaybeLocal<Value>();
  }

  return ret;
}

// Public MakeCallback()s

MaybeLocal<Value> MakeCallback(Isolate* isolate,
                               Local<Object> recv,
                               const char* method,
                               int argc,
                               Local<Value> argv[],
                               async_context asyncContext) {
  Local<String> method_string =
      String::NewFromUtf8(isolate, method, NewStringType::kNormal)
          .ToLocalChecked();
  return MakeCallback(isolate, recv, method_string, argc, argv, asyncContext);
}

MaybeLocal<Value> MakeCallback(Isolate* isolate,
                               Local<Object> recv,
                               Local<String> symbol,
                               int argc,
                               Local<Value> argv[],
                               async_context asyncContext) {
  // Check can_call_into_js() first because calling Get() might do so.
  Environment* env = Environment::GetCurrent(recv->CreationContext());
  CHECK_NOT_NULL(env);
  if (!env->can_call_into_js()) return Local<Value>();

  Local<Value> callback_v;
  if (!recv->Get(isolate->GetCurrentContext(), symbol).ToLocal(&callback_v))
    return Local<Value>();
  if (!callback_v->IsFunction()) {
    // This used to return an empty value, but Undefined() makes more sense
    // since no exception is pending here.
    return Undefined(isolate);
  }
  Local<Function> callback = callback_v.As<Function>();
  return MakeCallback(isolate, recv, callback, argc, argv, asyncContext);
}

MaybeLocal<Value> MakeCallback(Isolate* isolate,
                               Local<Object> recv,
                               Local<Function> callback,
                               int argc,
                               Local<Value> argv[],
                               async_context asyncContext) {
  // Observe the following two subtleties:
  //
  // 1. The environment is retrieved from the callback function's context.
  // 2. The context to enter is retrieved from the environment.
  //
  // Because of the AssignToContext() call in src/node_contextify.cc,
  // the two contexts need not be the same.
  Environment* env = Environment::GetCurrent(callback->CreationContext());
  CHECK_NOT_NULL(env);
  Context::Scope context_scope(env->context());
  MaybeLocal<Value> ret =
      InternalMakeCallback(env, recv, recv, callback, argc, argv, asyncContext);
  if (ret.IsEmpty() && env->async_callback_scope_depth() == 0) {
    // This is only for legacy compatibility and we may want to look into
    // removing/adjusting it.
    return Undefined(env->isolate());
  }
  return ret;
}

// Legacy MakeCallback()s

Local<Value> MakeCallback(Isolate* isolate,
                          Local<Object> recv,
                          const char* method,
                          int argc,
                          Local<Value>* argv) {
  EscapableHandleScope handle_scope(isolate);
  return handle_scope.Escape(
      MakeCallback(isolate, recv, method, argc, argv, {0, 0})
          .FromMaybe(Local<Value>()));
}

Local<Value> MakeCallback(Isolate* isolate,
                          Local<Object> recv,
                          Local<String> symbol,
                          int argc,
                          Local<Value>* argv) {
  EscapableHandleScope handle_scope(isolate);
  return handle_scope.Escape(
      MakeCallback(isolate, recv, symbol, argc, argv, {0, 0})
          .FromMaybe(Local<Value>()));
}

Local<Value> MakeCallback(Isolate* isolate,
                          Local<Object> recv,
                          Local<Function> callback,
                          int argc,
                          Local<Value>* argv) {
  EscapableHandleScope handle_scope(isolate);
  return handle_scope.Escape(
      MakeCallback(isolate, recv, callback, argc, argv, {0, 0})
          .FromMaybe(Local<Value>()));
}

}  // namespace node
