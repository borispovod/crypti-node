#include "node.h"
#include "node_async.h"

#include "env.h"
#include "env-inl.h"
#include "v8.h"
#include "uv.h"


#include <unistd.h>
#include <string.h>
#include <vector>

using namespace std;

#define CODIUS_ASYNC_IO_FD 3
#define CODIUS_MAGIC_BYTES 0xC0D105FE

static unsigned long int unique_id = 1;

namespace node {
namespace Async {

using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Handle;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::Persistent;
using v8::String;
using v8::Value;
using v8::TryCatch;
using v8::JSON;
using v8::Integer;
using v8::Number;

struct Async_req
{
  const char *data;
  size_t data_length;
  Isolate* isolate;
  unsigned int callback_id;

  Persistent<Function> callback;
};

uv_pipe_t stdin_pipe;
uv_pipe_t stdout_pipe;

Persistent<Function> pfn;

struct CBItem
{
    unsigned int callback_id;
    Persistent<Function> callback;
};

vector<Async_req *> cbs;


void OnMessageResponse(const FunctionCallbackInfo<Value>& args);

void AsyncAfter(uv_work_t* req, int something, const char *buf, size_t buf_len)
{
  Handle<Object> response;

  Async_req *data = ((struct Async_req*)req->data);

  // Parse the response.
  Local<String> response_str = String::NewFromUtf8(data->isolate, buf,
                                                   String::kNormalString, buf_len);
  Local<Object> global = data->isolate->GetCurrentContext()->Global();
  Handle<Object> JSON = global->Get(String::NewFromUtf8(
                                      data->isolate, "JSON"))->ToObject();
  Handle<Function> JSON_parse = Handle<Function>::Cast(JSON->Get(
                                  String::NewFromUtf8(data->isolate,
                                                      "parse")));
  Local<Value> parse_args[] = { response_str };
  response = Handle<Object> (JSON_parse->Call(JSON, 1, parse_args)->ToObject());
  Handle<Value> resp_err = response->Get(String::NewFromUtf8(data->isolate,
                                                                "error"));

  Local<Value> args[] = {
    resp_err,
    response->Get(String::NewFromUtf8(data->isolate, "result"))
  };

  TryCatch try_catch;

  Local<Function> callback_fn = Local<Function>::New(data->isolate, data->callback);
  callback_fn->Call(data->isolate->GetCurrentContext()->Global(), 2, args);

  if (try_catch.HasCaught())
  {
      FatalException(try_catch);
  }

  delete req;

  delete data;
}


void registerMessage(uv_work_t *req) {
    Async_req *data = ((struct Async_req*)req->data);

    write(4, data->data, strlen(data->data));
}

void after_registerMessage(uv_work_t *req, int status) {
    Async_req *data = ((struct Async_req*)req->data);

    // register callback
    cbs.push_back(data);
}

void PostMessage(Environment* env, const char *data, size_t data_length, unsigned int callback_id, Handle<Function> callback) {

  Async_req* request = new Async_req;

  request->data = data;
  request->data_length = data_length;
  request->isolate = env->isolate();
  request->callback.Reset(env->isolate(), callback);
  request->callback_id = callback_id;

  uv_work_t* req = new uv_work_t();
  req->data = request;
  uv_queue_work(env->event_loop(), req, registerMessage, after_registerMessage);
}

void AsyncMessage(uv_work_t* req, int something, const char *buf, size_t buf_len)
{
  Async_req *data = ((struct Async_req*)req->data);

  Local<Value> args[1] = {
      String::NewFromUtf8(data->isolate, buf)
    };

  TryCatch try_catch;

  Local<Function> callback_fn = Local<Function>::New(data->isolate, data->callback);
  callback_fn->Call(data->isolate->GetCurrentContext()->Global(), 1, args);

  if (try_catch.HasCaught())
  {
      FatalException(try_catch);
  }

  delete data;
}

static void OnMessage(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args.GetIsolate());
    HandleScope scope(env->isolate());

    if (args.Length() < 1) {
        return ThrowError(env->isolate(), "needs argument object and callback");
    }

    if (!args[0]->IsFunction()) {
        return ThrowError(env->isolate(), "argument should be a callback");
    }

   pfn.Reset(env->isolate(), args[0].As<Function>());
}

/*
    Pipes
*/

// Read
void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    *buf = uv_buf_init((char*) malloc(suggested_size), suggested_size);
}

void recieveWork(uv_work_t *req) {
}

void after_recieveWork(uv_work_t *req, int status) {
    Async_req *data = ((struct Async_req*)req->data);
    Local<Function> callback_fn = Local<Function>::New(data->isolate, pfn);

    Handle<Object> response;
    Local<String> response_str = String::NewFromUtf8(data->isolate, data->data, String::kNormalString, data->data_length);
    Local<Object> global = data->isolate->GetCurrentContext()->Global();
    Handle<Object> JSON = global->Get(String::NewFromUtf8(data->isolate, "JSON"))->ToObject();
    Handle<Function> JSON_parse = Handle<Function>::Cast(JSON->Get(String::NewFromUtf8(data->isolate, "parse")));

    Local<Value> parse_args[] = { response_str };
    response = Handle<Object> (JSON_parse->Call(JSON, 1, parse_args)->ToObject());


    Local<FunctionTemplate> tpl = FunctionTemplate::New(data->isolate, OnMessageResponse);
    Local<Function> callback =  tpl->GetFunction();

    Local<Value> args[] = {
        response,
        callback
    };

    v8::TryCatch try_catch;
    callback_fn->Call(global, 2, args);
    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }
}


void findCallback(uv_work_t *req) {
    Async_req *data = ((struct Async_req*)req->data);

    // find callback
    unsigned int cb_id = data->callback_id;

    Async_req *callData;
    for (auto &i : cbs) {
        if (i->callback_id == data->callback_id) {
            callData = i;
            break;
        }
    }

    data->callback.Reset(data->isolate, callData->callback);
}

void after_findCallback(uv_work_t *req, int status) {
    Async_req *data = ((struct Async_req*)req->data);

    Handle<Object> response;
    Local<String> response_str = String::NewFromUtf8(data->isolate, data->data, String::kNormalString, data->data_length);
    Local<Object> global = data->isolate->GetCurrentContext()->Global();
    Handle<Object> JSON = global->Get(String::NewFromUtf8(data->isolate, "JSON"))->ToObject();
    Handle<Function> JSON_parse = Handle<Function>::Cast(JSON->Get(String::NewFromUtf8(data->isolate, "parse")));

    Local<Value> parse_args[] = { response_str };
    response = Handle<Object> (JSON_parse->Call(JSON, 1, parse_args)->ToObject());



    Local<Value> args[] = {
        response->Get(String::NewFromUtf8(data->isolate, "error")),
        response->Get(String::NewFromUtf8(data->isolate, "response"))
    };


    Local<Function> callback_fn = Local<Function>::New(data->isolate, data->callback);
    callback_fn->Call(data->isolate->GetCurrentContext()->Global(), 2, args);

    v8::TryCatch try_catch;
    callback_fn->Call(global, 2, args);
    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }
}

void read_stdin(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    Isolate *isolate = Isolate::GetCurrent();
    Environment *env = Environment::GetCurrent(isolate->GetCurrentContext());

    if (nread < 0){
        if (nread == UV_EOF){
            uv_close((uv_handle_t *)&stdin_pipe, NULL);
            uv_close((uv_handle_t *)&stdout_pipe, NULL);
        }
    } else if (nread > 0) {
        // get json and type
        Local<String> message = String::NewFromUtf8(env->isolate(), (char*)buf->base,String::kNormalString, nread);
        Local<Object> global = isolate->GetCurrentContext()->Global();
        Handle<Object> JSON = global->Get(String::NewFromUtf8(env->isolate(), "JSON"))->ToObject();
        Handle<Function> JSON_parse = Handle<Function>::Cast(JSON->Get(String::NewFromUtf8(env->isolate(),"parse")));

        Local<Value> parse_args[] = { message };
        Handle<Object> response = Handle<Object>(JSON_parse->Call(JSON, 1, parse_args)->ToObject());
        Local<Value> typeValue = response->Get(String::NewFromUtf8(env->isolate(), "type"));

        if (typeValue->IsNull() || typeValue->IsUndefined()) {
            return ThrowError(env->isolate(), "needs type argument");
        }

        Local<String> type = typeValue->ToString();

        if (type->Equals(String::NewFromUtf8(env->isolate(), "crypti_call"))) {
            Local<Value> callback_id = response->Get(String::NewFromUtf8(env->isolate(), "callback_id"));

            if (callback_id->IsNull() || typeValue->IsUndefined()) {
                return ThrowError(env->isolate(), "needs callback_id argument");
            }

            if (!callback_id->IsNumber()) {
                return ThrowError(env->isolate(), "callback_id argument should be a number");
            }

            Local<Value> messageObj = response->Get(String::NewFromUtf8(env->isolate(), "message"));

            if (messageObj->IsNull() || messageObj->IsUndefined()) {
                return ThrowError(env->isolate(), "needs message argument");
            }

            if (!messageObj->IsObject()) {
                return ThrowError(env->isolate(), "message argument should be an object");
            }

            Async_req* request = new Async_req;
            request->data = buf->base;
            request->data_length = (size_t)nread;
            request->isolate = env->isolate();
            request->callback.Reset(env->isolate(), pfn);

            uv_work_t req;
            req.data = request;

            // call or response
            uv_queue_work(env->event_loop(), &req, recieveWork, after_recieveWork);
        } else if (type->Equals(String::NewFromUtf8(env->isolate(), "crypti_response"))) {
            Local<Value> callback_id = response->Get(String::NewFromUtf8(env->isolate(), "callback_id"));

            if (callback_id->IsNull() || typeValue->IsUndefined()) {
                return ThrowError(env->isolate(), "needs callback_id argument");
            }

            if (!callback_id->IsNumber()) {
                return ThrowError(env->isolate(), "callback_id argument should be a number");
            }

            Local<Value> responseObj = response->Get(String::NewFromUtf8(env->isolate(), "response"));

            if (responseObj->IsNull() || responseObj->IsUndefined()) {
                return ThrowError(env->isolate(), "needs response argument");
            }

            if (!responseObj->IsObject()) {
                return ThrowError(env->isolate(), "response argument should be an object");
            }

            Local<Value> errorObj = response->Get(String::NewFromUtf8(env->isolate(), "error"));

            if (!errorObj->IsNull() && !errorObj->IsUndefined()) {
                if (!errorObj->IsString()) {
                    return ThrowError(env->isolate(), "response argument should be an string");
                }
            }

            // process response
            Async_req* request = new Async_req;
            request->data = buf->base;
            request->data_length = (size_t)nread;
            request->isolate = env->isolate();
            request->callback_id = callback_id->ToNumber()->Value();

            uv_work_t req;
            req.data = request;

            // find callback and call
            uv_queue_work(env->event_loop(), &req, findCallback, after_findCallback);
        } else {
            return ThrowError(env->isolate(), "unknown call type argument");
        }
    }

    if (buf->base)
        free(buf->base);
}

void StartListen(Environment *env) {
   uv_pipe_init(env->event_loop(), &stdin_pipe, 0);
   uv_pipe_open(&stdin_pipe, 3);

   uv_read_start((uv_stream_t*)&stdin_pipe, alloc_buffer, read_stdin);
}


void sendWork(uv_work_t *req) {
    Async_req *data = ((struct Async_req*)req->data);
    write(4, data->data, data->data_length);
}

void after_sendWork(uv_work_t *req, int status) {}

void OnMessageResponse(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args.GetIsolate());
    HandleScope scope(env->isolate());

    if (args.Length() < 1) {
        return ThrowError(env->isolate(), "needs argument error");
    }

    if (!args[0]->IsNull()) {
        if (!args[0]->IsString()) {
            return ThrowError(env->isolate(), "error argument should be a string");
        }

        Local<Object> response = Object::New(env->isolate());
        Handle<String> error = Handle<String>::Cast(args[0]);
        response->Set(String::NewFromUtf8(env->isolate(), "error"), error);
        response->Set(String::NewFromUtf8(env->isolate(), "type"), String::NewFromUtf8(env->isolate(), "dapp_response"));

        // get id and find callback
        Local<Value> callback_id = response->Get(String::NewFromUtf8(env->isolate(), "callback_id"));

        if (callback_id->IsNull()) {
            return ThrowError(env->isolate(), "callback id of response should be provided");
        }

        if (!callback_id->IsNumber()) {
            return ThrowError(env->isolate(), "callback id of response should be a number");
        }

        Local<Object> global = env->context()->Global();

        Handle<Object> JSON = global->Get(String::NewFromUtf8(
                                            env->isolate(), "JSON"))->ToObject();

        Handle<Function> JSON_stringify = Handle<Function>::Cast(JSON->Get(
                                            String::NewFromUtf8(env->isolate(),
                                                                "stringify")));
        Local<Value> stringify_args[] = { response };
        Local<String> str = JSON_stringify->Call(JSON, 1, stringify_args)->ToString();
        String::Utf8Value message(str);

        Async_req* request = new Async_req;
        request->data = *message;
        request->data_length = message.length();
        request->isolate = env->isolate();

        uv_work_t req;
        req.data = request;
        uv_queue_work(env->event_loop(), &req, sendWork, after_sendWork);
    } else {
        if (args.Length() < 2) {
            return ThrowError(env->isolate(), "needs argument error and second agrument response");
        }

        if (!args[1]->IsObject()) {
            return ThrowError(env->isolate(), "error argument should be a object");
        }

        Handle<Object> response = Handle<Object>::Cast(args[1]);

        if (args[0]->IsString()) {
            Handle<String> error = Handle<String>::Cast(args[0]);
            response->Set(String::NewFromUtf8(env->isolate(), "error"), error);
        } else if (!args[0]->IsNull()) {
            return ThrowError(env->isolate(), "error argument should be a string or null");
        }

        response->Set(String::NewFromUtf8(env->isolate(), "type"), String::NewFromUtf8(env->isolate(), "dapp_response"));

        // get id and find callback
        Local<Value> callback_id = response->Get(String::NewFromUtf8(env->isolate(), "callback_id"));

        if (callback_id->IsNull()) {
            return ThrowError(env->isolate(), "callback id of response should be provided");
        }

        if (!callback_id->IsNumber()) {
            return ThrowError(env->isolate(), "callback id of response should be a number");
        }

        Local<Object> global = env->context()->Global();

        Handle<Object> JSON = global->Get(String::NewFromUtf8(
                                            env->isolate(), "JSON"))->ToObject();

        Handle<Function> JSON_stringify = Handle<Function>::Cast(JSON->Get(
                                            String::NewFromUtf8(env->isolate(),
                                                                "stringify")));
        Local<Value> stringify_args[] = { response };
        Local<String> str = JSON_stringify->Call(JSON, 1, stringify_args)->ToString();
        String::Utf8Value message(str);

        Async_req* request = new Async_req;
        request->data = *message;
        request->data_length = message.length();
        request->isolate = env->isolate();

        uv_work_t req;
        req.data = request;
        uv_queue_work(env->event_loop(), &req, sendWork, after_sendWork);

    }
}

//////////

static void PostMessage(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  if (args.Length() < 2)
    return ThrowError(env->isolate(), "needs argument object and callback");

  if (!args[1]->IsFunction()) {
    return ThrowError(env->isolate(), "second argument should be a callback");
  }

  if (args[0]->IsObject()) {
    Handle<Object> messageCall = Handle<Object>::Cast(args[0]);

    Local<Value> messageObj = messageCall->Get(String::NewFromUtf8(env->isolate(), "message"));

    if (messageObj->IsNull() || messageObj->IsUndefined()) {
        return ThrowError(env->isolate(), "needs message argument");
    }

    if (!messageObj->IsObject()) {
        return ThrowError(env->isolate(), "message argument should be an object");
    }

    unsigned int cb_id = unique_id++;
    messageCall->Set(String::NewFromUtf8(env->isolate(), "type"), String::NewFromUtf8(env->isolate(), "dapp_call"));
    messageCall->Set(String::NewFromUtf8(env->isolate(), "callback_id"), Integer::New(env->isolate(), cb_id)->ToString());

    // Stringify the JSON
    Local<Object> global = env->context()->Global();
    Handle<Object> JSON = global->Get(String::NewFromUtf8(
                                        env->isolate(), "JSON"))->ToObject();
    Handle<Function> JSON_stringify = Handle<Function>::Cast(JSON->Get(
                                        String::NewFromUtf8(env->isolate(),
                                                            "stringify")));
    Local<Value> stringify_args[] = { messageCall };
    Local<String> str = JSON_stringify->Call(JSON, 1, stringify_args)->ToString();
    const int length = str->Utf8Length() + 1;  // Add one for trailing zero byte.
    uint8_t* buffer = new uint8_t[length];
    v8::String::Utf8Value m(str);
    str->WriteOneByte(buffer, 0, length);

    //const char *msg= "{\"test\":\"123\"}";
    //char *msg = *m;
    PostMessage(env, (char*)buffer, strlen((char*)buffer), cb_id, Handle<Function>::Cast(args[1]));
  } else {
    return ThrowError(env->isolate(), "first argument should be a message object");
  }
}

void Initialize(Handle<Object> target,
                Handle<Value> unused,
                Handle<Context> context) {
 Environment *env = Environment::GetCurrent(context);

 NODE_SET_METHOD(target, "postMessage", PostMessage);
 NODE_SET_METHOD(target, "onMessage", OnMessage);

 // Start Listen
 StartListen(env);
}


}  // namespace Async
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(async, node::Async::Initialize)
