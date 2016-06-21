#include <pthread.h>
#include <iostream>
#include <memory>
#include <v8.h>

using namespace std;
using namespace v8;

typedef struct thread_status {
    auto_ptr<string> error;
    auto_ptr<string> result;
} thread_status;

class V8Thread {

private:
    pthread_t thread;
    Isolate *isolate;
    Persistent<Context> context;
    Persistent<Function> function;

public:
    V8Thread(const char *code, const char *origin);
    ~V8Thread();

    static void install(Handle<Object> global);

    static Handle<Value> _create(const Arguments& args);
    static Handle<Value> _start(const Arguments& args);
    static Handle<Value> _join(const Arguments& args);
    static void _destroy(Persistent<Value> object, void* data);

    void start(const char* code);
    thread_status* run(const char* arg);
    thread_status* join();
};
