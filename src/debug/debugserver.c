#include "moar.h"
#include "platform/threads.h"

#include <stdbool.h>
#include "cmp.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET Socket;
    #define sa_family_t unsigned int
#else
    #include "unistd.h"
    #include <sys/socket.h>
    #include <sys/un.h>

    typedef int Socket;
    #define closesocket close
#endif

typedef enum {
    MT_MessageTypeNotUnderstood,
    MT_ErrorProcessingMessage,
    MT_OperationSuccessful,
    MT_IsExecutionSuspendedRequest,
    MT_IsExecutionSuspendedResponse,
    MT_SuspendAll,
    MT_ResumeAll,
    MT_SuspendOne,
    MT_ResumeOne,
    MT_ThreadStarted,
    MT_ThreadEnded,
    MT_ThreadListRequest,
    MT_ThreadListResponse,
    MT_ThreadStackTraceRequest,
    MT_ThreadStackTraceResponse,
    MT_SetBreakpointRequest,
    MT_SetBreakpointConfirmation,
    MT_BreakpointNotification,
    MT_ClearBreakpoint,
    MT_ClearAllBreakpoints,
    MT_StepInto,
    MT_StepOver,
    MT_StepOut,
    MT_StepCompleted,
    MT_ReleaseHandles,
    MT_HandleResult,
    MT_ContextHandle,
    MT_ContextLexicalsRequest,
    MT_ContextLexicalsResponse,
    MT_OuterContextRequest,
    MT_CallerContextRequest,
    MT_CodeObjectHandle,
    MT_ObjectAttributesRequest,
    MT_ObjectAttributesResponse,
    MT_DecontainerizeHandle,
    MT_FindMethod,
    MT_Invoke,
    MT_InvokeResult,
    MT_UnhandledException,
} message_type;

typedef enum {
    ArgKind_Handle,
    ArgKind_Integer,
    ArgKind_Num,
    ArgKind_String,
} argument_kind;

typedef struct {
    MVMuint8 arg_kind;
    union {
        MVMint64 i;
        MVMnum64 n;
        char *s;
        MVMint64 o;
    } arg_u;
} argument_data;

typedef enum {
    FS_type      = 1,
    FS_id        = 2,
    FS_thread_id = 4,
    FS_file      = 8,
    FS_line      = 16,
    FS_suspend   = 32,
    FS_stacktrace = 64,
    /* handle_count is just bookkeeping */
    FS_handles    = 128,
    FS_handle_id  = 256,
    FS_frame_number = 512,
    FS_arguments    = 1024,
} fields_set;

typedef struct {
    MVMuint16 type;
    MVMuint64 id;

    MVMuint32 thread_id;

    char *file;
    MVMuint64 line;

    MVMuint8  suspend;
    MVMuint8  stacktrace;

    MVMuint16 handle_count;
    MVMuint64 *handles;

    MVMuint64 handle_id;

    MVMuint32 frame_number;

    MVMuint32 argument_count;
    argument_data *arguments;

    MVMuint8  parse_fail;
    const char *parse_fail_message;

    fields_set fields_set;
} request_data;

#define REQUIRE(field, message) do { if(!(data->fields_set & (field))) { data->parse_fail = 1; data->parse_fail_message = (message); return 0; }; accepted = accepted | (field); } while (0)

MVMuint8 check_requirements(request_data *data) {
    fields_set accepted = FS_id | FS_type;

    REQUIRE(FS_id, "An id field is required");
    REQUIRE(FS_type, "A type field is required");
    switch (data->type) {
        case MT_IsExecutionSuspendedRequest:
        case MT_SuspendAll:
        case MT_ResumeAll:
        case MT_ThreadListRequest:
        case MT_ClearAllBreakpoints:
            /* All of these messages only take id and type */
            break;

        case MT_SuspendOne:
        case MT_ResumeOne:
        case MT_ThreadStackTraceRequest:
        case MT_StepInto:
        case MT_StepOver:
        case MT_StepOut:
            REQUIRE(FS_thread_id, "A thread field is required");
            break;

        case MT_SetBreakpointRequest:
            REQUIRE(FS_suspend, "A suspend field is required");
            REQUIRE(FS_stacktrace, "A stacktrace field is required");
            /* Fall-Through */
        case MT_ClearBreakpoint:
            REQUIRE(FS_file, "A file field is required");
            REQUIRE(FS_line, "A line field is required");
            break;

        case MT_ReleaseHandles:
            REQUIRE(FS_handles, "A handles field is required");

        case MT_FindMethod:
            /* TODO we've got to have some name field or something */
            /* Fall-Through */
        case MT_DecontainerizeHandle:
            REQUIRE(FS_thread_id, "A thread field is required");
            /* Fall-Through */
        case MT_ContextLexicalsRequest:
        case MT_OuterContextRequest:
        case MT_CallerContextRequest:
        case MT_ObjectAttributesRequest:
            REQUIRE(FS_handle_id, "A handle field is required");
            break;

        case MT_ContextHandle:
        case MT_CodeObjectHandle:
            REQUIRE(FS_thread_id, "A thread field is required");
            REQUIRE(FS_frame_number, "A frame field is required");
            break;

        case MT_Invoke:
            REQUIRE(FS_handle_id, "A handle field is required");
            REQUIRE(FS_thread_id, "A thread field is required");
            REQUIRE(FS_arguments, "An arguments field is required");
            break;

        default:
            break;
    }

    if (data->fields_set != accepted) {
        data->parse_fail = 1;
        data->parse_fail_message = "Too many keys in message";
    }
}

static void send_greeting(Socket *sock) {
    char buffer[24] = "MOARVM-REMOTE-DEBUG\0";
    MVMuint32 version = htobe16(1);

    MVMuint16 *verptr = (MVMuint16 *)(&buffer[strlen("MOARVM-REMOTE-DEBUG") + 1]);
    *verptr = version;
    verptr++;
    *verptr = version;
    send(*sock, buffer, 24, 0);
}

static int receive_greeting(Socket *sock) {
    const char *expected = "MOARVM-REMOTE-CLIENT-OK";
    char buffer[strlen(expected) + 1];
    int received = 0;

    memset(buffer, 0, sizeof(buffer));

    received = recv(*sock, buffer, sizeof(buffer), 0);
    if (received != sizeof(buffer)) {
        return 0;
    }
    if (memcmp(buffer, expected, sizeof(buffer)) == 0) {
        return 1;
    }
    return 0;
}

static void communicate_error(cmp_ctx_t *ctx, request_data *argument) {
    fprintf(stderr, "communicating an error\n");
    cmp_write_map(ctx, 2);
    cmp_write_str(ctx, "id", 2);
    cmp_write_integer(ctx, argument->id);
    cmp_write_str(ctx, "type", 4);
    cmp_write_integer(ctx, 0);
}

static void communicate_success(cmp_ctx_t *ctx, request_data *argument) {
    fprintf(stderr, "communicating success\n");
    cmp_write_map(ctx, 2);
    cmp_write_str(ctx, "id", 2);
    cmp_write_integer(ctx, argument->id);
    cmp_write_str(ctx, "type", 4);
    cmp_write_integer(ctx, 2);
}

static MVMThread *find_thread_by_id(MVMInstance *vm, MVMint32 id) {
    MVMThread *cur_thread = 0;

    fprintf(stderr, "looking for thread number %d\n", id);

    if (id == vm->debugserver_thread_id) {
        return NULL;
    }

    uv_mutex_lock(&vm->mutex_threads);
    cur_thread = vm->threads;
    while (cur_thread) {
        fprintf(stderr, "%d ", cur_thread->body.thread_id);
        if (cur_thread->body.thread_id == id) {
            break;
        }
        cur_thread = cur_thread->body.next;
    }
    fprintf(stderr, "\n");
    uv_mutex_unlock(&vm->mutex_threads);
    return cur_thread;
}

static MVMint32 request_thread_suspends(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument, MVMThread *thread) {
    MVMThread *to_do = thread ? thread : find_thread_by_id(dtc->instance, argument->thread_id);
    MVMThreadContext *tc = to_do ? to_do->body.tc : NULL;

    if (!tc)
        return 1;

    MVM_gc_mark_thread_blocked(dtc);
    while (1) {
        if (MVM_cas(&tc->gc_status, MVMGCStatus_NONE, MVMGCStatus_INTERRUPT | MVMSuspendState_SUSPEND_REQUEST)
                == MVMGCStatus_NONE) {
            break;
        }
        MVM_platform_thread_yield();
    }

    while (1) {
        if (MVM_load(&tc->gc_status) != (MVMGCStatus_UNABLE | MVMSuspendState_SUSPENDED)) {
            MVM_platform_thread_yield();
        } else {
            break;
        }
    }
    MVM_gc_mark_thread_unblocked(dtc);

    fprintf(stderr, "thread successfully suspended\n");

    return 0;
}

static MVMint32 request_thread_resumes(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument, MVMThread *thread) {
    MVMInstance *vm = dtc->instance;
    MVMThread *to_do = thread ? thread : find_thread_by_id(vm, argument->thread_id);
    MVMThreadContext *tc = to_do ? to_do->body.tc : NULL;

    if (!tc)
        return 1;

    if (MVM_load(&tc->gc_status) != (MVMGCStatus_UNABLE | MVMSuspendState_SUSPENDED)) {
        return 1;
    }

    MVM_gc_mark_thread_blocked(dtc);

    while(1) {
        AO_t current = MVM_cas(&tc->gc_status, MVMGCStatus_UNABLE | MVMSuspendState_SUSPENDED, MVMGCStatus_UNABLE);
        if (current == (MVMGCStatus_UNABLE | MVMSuspendState_SUSPENDED)) {
            /* Success! We signalled the thread and can now tell it to
             * mark itself unblocked, which takes care of any looming GC
             * and related business. */
            uv_cond_broadcast(&vm->debugserver_tell_threads);
            break;
        } else if ((current & MVMGCSTATUS_MASK) == MVMGCStatus_STOLEN) {
            uv_mutex_lock(&tc->instance->mutex_gc_orchestrate);
            if (tc->instance->in_gc) {
                uv_cond_wait(&tc->instance->cond_blocked_can_continue,
                    &tc->instance->mutex_gc_orchestrate);
            }
            uv_mutex_unlock(&tc->instance->mutex_gc_orchestrate);
        }
    }

    MVM_gc_mark_thread_unblocked(dtc);

    fprintf(stderr, "success resuming thread\n");

    return 0;
}

static MVMint32 request_all_threads_resume(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument) {
    MVMInstance *vm = dtc->instance;
    MVMThread *cur_thread = 0;

    uv_mutex_lock(&vm->mutex_threads);
    cur_thread = vm->threads;
    while (cur_thread) {
        if (cur_thread != dtc->thread_obj) {
            if (MVM_load(&cur_thread->body.tc->gc_status) == (MVMGCStatus_UNABLE | MVMSuspendState_SUSPENDED)) {
                request_thread_resumes(dtc, ctx, argument, cur_thread);
            }
        }
        cur_thread = cur_thread->body.next;
    }
    uv_mutex_unlock(&vm->mutex_threads);
}

static MVMint32 request_thread_stacktrace(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument, MVMThread *thread) {
    MVMThread *to_do = thread ? thread : find_thread_by_id(dtc->instance, argument->thread_id);

    if (!to_do)
        return 1;

    if ((to_do->body.tc->gc_status & MVMGCSTATUS_MASK) != MVMGCStatus_UNABLE) {
        return 1;
    }

    {

    MVMThreadContext *tc = to_do->body.tc;
    MVMuint64 stack_size = 0;

    MVMFrame *cur_frame = tc->cur_frame;

    while (cur_frame != NULL) {
        stack_size++;
        cur_frame = cur_frame->caller;
    }

    fprintf(stderr, "dumping a stack trace of %d frames\n", stack_size);

    cmp_write_map(ctx, 3);
    cmp_write_str(ctx, "id", 2);
    cmp_write_integer(ctx, argument->id);
    cmp_write_str(ctx, "type", 4);
    cmp_write_integer(ctx, MT_ThreadStackTraceResponse);
    cmp_write_str(ctx, "frames", 6);

    cmp_write_array(ctx, stack_size);

    cur_frame = tc->cur_frame;
    stack_size = 0; /* To check if we've got the topmost frame or not */

    while (cur_frame != NULL) {
        MVMString *bc_filename = cur_frame->static_info->body.cu->body.filename;
        MVMString *name     = cur_frame->static_info->body.name;

        MVMuint8 *cur_op = stack_size != 0 ? cur_frame->return_address : *(tc->interp_cur_op);
        MVMuint32 offset = cur_op - MVM_frame_effective_bytecode(cur_frame);
        MVMBytecodeAnnotation *annot = MVM_bytecode_resolve_annotation(tc, &cur_frame->static_info->body,
                                          offset > 0 ? offset - 1 : 0);

        MVMint32 line_number = annot ? annot->line_number : 1;
        MVMint16 string_heap_index = annot ? annot->filename_string_heap_index : 1;

        char *tmp1 = annot && string_heap_index < cur_frame->static_info->body.cu->body.num_strings
            ? MVM_string_utf8_encode_C_string(tc, MVM_cu_string(tc,
                    cur_frame->static_info->body.cu, string_heap_index))
            : NULL;
        char *filename_c = bc_filename
            ? MVM_string_utf8_encode_C_string(tc, bc_filename)
            : NULL;
        char *name_c = name
            ? MVM_string_utf8_encode_C_string(tc, name)
            : NULL;

        char *debugname = cur_frame->code_ref ? MVM_6model_get_debug_name(tc, cur_frame->code_ref) : "";

        cmp_write_map(ctx, 5);
        cmp_write_str(ctx, "file", 4);
        cmp_write_str(ctx, tmp1, tmp1 ? strlen(tmp1) : 0);
        cmp_write_str(ctx, "line", 4);
        cmp_write_integer(ctx, line_number);
        cmp_write_str(ctx, "bytecode_file", 13);
        if (bc_filename)
            cmp_write_str(ctx, filename_c, strlen(filename_c));
        else
            cmp_write_nil(ctx);
        cmp_write_str(ctx, "name", 4);
        cmp_write_str(ctx, name_c, name_c ? strlen(name_c) : 0);
        cmp_write_str(ctx, "type", 4);
        cmp_write_str(ctx, debugname, strlen(debugname));

        if (bc_filename)
            MVM_free(filename_c);
        if (name)
            MVM_free(name_c);
        if (tmp1)
            MVM_free(tmp1);

        cur_frame = cur_frame->caller;
        stack_size++;
    }

    }

    return 0;
}

static void send_thread_info(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument) {
    MVMInstance *vm = dtc->instance;
    MVMint32 threadcount = 0;
    MVMThread *cur_thread;
    char infobuf[32] = "THL";

    uv_mutex_lock(&vm->mutex_threads);
    cur_thread = vm->threads;
    while (cur_thread) {
        threadcount++;
        cur_thread = cur_thread->body.next;
    }

    cmp_write_map(ctx, 3);
    cmp_write_str(ctx, "id", 2);
    cmp_write_integer(ctx, argument->id);
    cmp_write_str(ctx, "type", 4);
    cmp_write_integer(ctx, MT_ThreadListResponse);
    cmp_write_str(ctx, "threads", 7);

    cmp_write_array(ctx, threadcount);

    cur_thread = vm->threads;
    while (cur_thread) {
        cmp_write_map(ctx, 5);

        cmp_write_str(ctx, "thread", 6);
        cmp_write_integer(ctx, cur_thread->body.thread_id);

        cmp_write_str(ctx, "native_id", 9);
        cmp_write_integer(ctx, cur_thread->body.native_thread_id);

        cmp_write_str(ctx, "app_lifetime", 12);
        cmp_write_bool(ctx, cur_thread->body.app_lifetime);

        cmp_write_str(ctx, "suspended", 9);
        cmp_write_bool(ctx, (MVM_load(&cur_thread->body.tc->gc_status) & MVMSUSPENDSTATUS_MASK) == MVMSuspendState_SUSPENDED);

        cmp_write_str(ctx, "num_locks", 9);
        cmp_write_integer(ctx, cur_thread->body.tc->num_locks);

        cur_thread = cur_thread->body.next;
    }
    uv_mutex_unlock(&vm->mutex_threads);
}

static MVMuint64 allocate_handle(MVMThreadContext *dtc, MVMObject *target) {
    if (!target) {
        return 0;
    } else {
        MVMDebugServerHandleTable *dht = dtc->instance->debug_handle_table;

        MVMuint64 id = dht->next_id++;

        if (dht->used + 1 > dht->allocated) {
            if (dht->allocated < 8192)
                dht->allocated *= 2;
            else
                dht->allocated += 1024;
            dht->entries = MVM_realloc(dht->entries, sizeof(MVMDebugServerHandleTableEntry) * dht->allocated);
        }

        dht->entries[dht->used].id = id;
        dht->entries[dht->used].target = target;
        dht->used++;

        return id;
    }
}

static MVMuint64 allocate_and_send_handle(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument, MVMObject *target) {
    MVMuint64 id = allocate_handle(dtc, target);
    cmp_write_map(ctx, 3);
    cmp_write_str(ctx, "id", 2);
    cmp_write_integer(ctx, argument->id);
    cmp_write_str(ctx, "type", 4);
    cmp_write_integer(ctx, MT_HandleResult);
    cmp_write_str(ctx, "handle", 6);
    cmp_write_integer(ctx, id);

    return id;
}

static MVMObject *find_handle_target(MVMThreadContext *dtc, MVMuint64 id) {
    MVMDebugServerHandleTable *dht = dtc->instance->debug_handle_table;
    MVMuint32 index;

    for (index = 0; index < dht->used; index++) {
        if (dht->entries[index].id == id)
            return dht->entries[index].target;
    }
    return NULL;
}

static MVMint32 create_context_or_code_obj_debug_handle(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument, MVMThread *thread) {
    MVMInstance *vm = dtc->instance;
    MVMThread *to_do = thread ? thread : find_thread_by_id(vm, argument->thread_id);

    if (!to_do)
        return 1;

    if ((to_do->body.tc->gc_status & MVMGCSTATUS_MASK) != MVMGCStatus_UNABLE) {
        return 1;
    }

    {

    MVMFrame *cur_frame = to_do->body.tc->cur_frame;
    MVMuint32 frame_idx;

    for (frame_idx = 0;
            cur_frame && frame_idx < argument->frame_number;
            frame_idx++, cur_frame = cur_frame->caller) { }

    if (!cur_frame) {
        fprintf(stderr, "couldn't create context/coderef handle: no such frame %d\n", argument->frame_number);
        return 1;
    }

    if (argument->type == MT_ContextHandle) {
        MVMROOT(dtc, cur_frame, {
            allocate_and_send_handle(dtc, ctx, argument, MVM_frame_context_wrapper(to_do->body.tc, cur_frame));
        });
    } else if (argument->type == MT_CodeObjectHandle) {
        allocate_and_send_handle(dtc, ctx, argument, cur_frame->code_ref);
    } else {
        fprintf(stderr, "Did not expect to see create_context_or_code_obj_debug_handle called with a %d type\n", argument->type);
        return 1;
    }

    }

    return 0;
}

static MVMint32 create_caller_context_debug_handle(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument, MVMThread *thread) {
    MVMObject *this_ctx = argument->handle_id
        ? find_handle_target(dtc, argument->handle_id)
        : dtc->instance->VMNull;

    MVMFrame *frame;
    if (!IS_CONCRETE(this_ctx) || REPR(this_ctx)->ID != MVM_REPR_ID_MVMContext) {
        fprintf(stderr, "caller context handle must refer to a definite MVMContext object\n");
        return 1;
    }
    if ((frame = ((MVMContext *)this_ctx)->body.context->caller))
        this_ctx = MVM_frame_context_wrapper(dtc, frame);

    allocate_and_send_handle(dtc, ctx, argument, this_ctx);
    return 0;
}

static MVMint32 request_context_lexicals(MVMThreadContext *dtc, cmp_ctx_t *ctx, request_data *argument) {
    MVMObject *this_ctx = argument->handle_id
        ? find_handle_target(dtc, argument->handle_id)
        : dtc->instance->VMNull;
    MVMStaticFrame *static_info;
    MVMLexicalRegistry *lexical_names;

    MVMFrame *frame;
    if (MVM_is_null(dtc, this_ctx) || !IS_CONCRETE(this_ctx) || REPR(this_ctx)->ID != MVM_REPR_ID_MVMContext) {
        fprintf(stderr, "getting lexicals: context handle must refer to a definite MVMContext object\n");
        return 1;
    }
    if (!(frame = ((MVMContext *)this_ctx)->body.context)) {
        fprintf(stderr, "context doesn't have a frame?!\n");
        return 1;
    }

    static_info = frame->static_info;
    lexical_names = static_info->body.lexical_names;
    if (lexical_names) {
        MVMLexicalRegistry *entry, *tmp;
        unsigned bucket_tmp;
        MVMuint64 lexcount = HASH_CNT(hash_handle, lexical_names);

        cmp_write_map(ctx, 3);
        cmp_write_str(ctx, "id", 2);
        cmp_write_integer(ctx, argument->id);
        cmp_write_str(ctx, "type", 4);
        cmp_write_integer(ctx, MT_ContextLexicalsResponse);

        cmp_write_str(ctx, "lexicals", 8);
        cmp_write_map(ctx, lexcount);

        fprintf(stderr, "will write %d lexicals\n", lexcount);

        HASH_ITER(hash_handle, lexical_names, entry, tmp, bucket_tmp) {
            MVMuint16 lextype = static_info->body.lexical_types[entry->value];
            MVMRegister *result = &frame->env[entry->value];
            char *c_key_name = MVM_string_utf8_encode_C_string(dtc, entry->key);

            cmp_write_str(ctx, c_key_name, strlen(c_key_name));

            MVM_free(c_key_name);

            if (lextype == MVM_reg_obj) { /* Object */
                char *debugname;

                if (!result->o) {
                    /* XXX this can't allocate? */
                    MVM_frame_vivify_lexical(dtc, frame, entry->value);
                }

                cmp_write_map(ctx, 5);

                cmp_write_str(ctx, "kind", 4);
                cmp_write_str(ctx, "obj", 3);

                cmp_write_str(ctx, "handle", 6);
                cmp_write_integer(ctx, allocate_handle(dtc, result->o));

                debugname = MVM_6model_get_debug_name(dtc, result->o);

                cmp_write_str(ctx, "type", 4);
                cmp_write_str(ctx, debugname, strlen(debugname));

                cmp_write_str(ctx, "concrete", 8);
                cmp_write_bool(ctx, IS_CONCRETE(result->o));

                cmp_write_str(ctx, "container", 9);
                cmp_write_bool(ctx, STABLE(result->o)->container_spec == NULL ? 0 : 1);
            } else {
                cmp_write_map(ctx, 2);

                cmp_write_str(ctx, "kind", 4);
                cmp_write_str(ctx,
                        lextype == MVM_reg_int64 ? "int" :
                        lextype == MVM_reg_num32 ? "num" :
                        lextype == MVM_reg_str   ? "str" :
                        "???", 3);

                cmp_write_str(ctx, "value", 5);
                if (lextype == MVM_reg_int64) {
                    cmp_write_integer(ctx, result->i64);
                } else if (lextype == MVM_reg_num64) {
                    cmp_write_double(ctx, result->n64);
                } else if (lextype == MVM_reg_str) {
                    char *c_value = MVM_string_utf8_encode_C_string(dtc, result->s);
                    cmp_write_str(ctx, c_value, strlen(c_value));
                    MVM_free(c_value);
                } else {
                    fprintf(stderr, "what lexical type is %d supposed to be?\n", lextype);
                    cmp_write_nil(ctx);
                }
            }
            fprintf(stderr, "wrote a lexical\n");
        }
    } else {
        cmp_write_map(ctx, 3);
        cmp_write_str(ctx, "id", 2);
        cmp_write_integer(ctx, argument->id);
        cmp_write_str(ctx, "type", 4);
        cmp_write_integer(ctx, MT_ContextLexicalsResponse);

        cmp_write_str(ctx, "lexicals", 8);
        cmp_write_map(ctx, 0);
    }
    fprintf(stderr, "done writing lexicals\n");
    return 0;
}

static bool socket_reader(cmp_ctx_t *ctx, void *data, size_t limit) {
    if (recv(*((Socket*)ctx->buf), data, limit, 0) == -1)
        return 0;
    return 1;
}

static size_t socket_writer(cmp_ctx_t *ctx, const void *data, size_t count) {
    if (send(*(Socket*)ctx->buf, data, count, 0) == -1)
        return 0;
    return 1;
}

static bool is_valid_int(cmp_object_t *obj, MVMint64 *result) {
    switch (obj->type) {
        case CMP_TYPE_POSITIVE_FIXNUM:
        case CMP_TYPE_UINT8:
            *result = obj->as.u8;
            break;
        case CMP_TYPE_UINT16:
            *result = obj->as.u16;
            break;
        case CMP_TYPE_UINT32:
            *result = obj->as.u32;
            break;
        case CMP_TYPE_UINT64:
            *result = obj->as.u64;
            break;
        case CMP_TYPE_NEGATIVE_FIXNUM:
        case CMP_TYPE_SINT8:
            *result = obj->as.s8;
            break;
        case CMP_TYPE_SINT16:
            *result = obj->as.s16;
            break;
        case CMP_TYPE_SINT32:
            *result = obj->as.s32;
            break;
        case CMP_TYPE_SINT64:
            *result = obj->as.s64;
            break;
        default:
            return 0;
    }
    return 1;
}

#define CHECK(operation, message) do { if(!(operation)) { data->parse_fail = 1; data->parse_fail_message = (message);fprintf(stderr, "%s", cmp_strerror(ctx)); return 0; } } while(0)
#define FIELD_FOUND(field, duplicated_message) do { if(data->fields_set & (field)) { data->parse_fail = 1; data->parse_fail_message = duplicated_message;  return 0; }; field_to_set = (field); } while (0)

MVMint32 parse_message_map(cmp_ctx_t *ctx, request_data *data) {
    MVMuint32 map_size = 0;
    MVMuint32 i;

    memset(data, 0, sizeof(request_data));

    CHECK(cmp_read_map(ctx, &map_size), "Couldn't read envelope map");

    for (i = 0; i < map_size; i++) {
        char key_str[16];
        MVMuint32 str_size = 16;

        fields_set field_to_set = 0;
        MVMuint32  type_to_parse = 0;

        CHECK(cmp_read_str(ctx, key_str, &str_size), "Couldn't read string key");

        if (strncmp(key_str, "type", 15) == 0) {
            FIELD_FOUND(FS_type, "type field duplicated");
            type_to_parse = 1;
        } else if (strncmp(key_str, "id", 15) == 0) {
            FIELD_FOUND(FS_id, "id field duplicated");
            type_to_parse = 1;
        } else if (strncmp(key_str, "thread", 15) == 0) {
            FIELD_FOUND(FS_thread_id, "thread field duplicated");
            type_to_parse = 1;
        } else if (strncmp(key_str, "frame", 15) == 0) {
            FIELD_FOUND(FS_frame_number, "frame number field duplicated");
            type_to_parse = 1;
        } else if (strncmp(key_str, "handle", 15) == 0) {
            FIELD_FOUND(FS_handle_id, "handle field duplicated");
            type_to_parse = 1;
        } else {
            fprintf(stderr, "the hell is a %s?\n", key_str);
            data->parse_fail = 1;
            data->parse_fail_message = "Unknown field encountered (NYI or protocol violation)";
            return 0;
        }

        if (type_to_parse == 1) {
            cmp_object_t object;
            MVMuint64 result;
            CHECK(cmp_read_object(ctx, &object), "Couldn't read value for a key");
            CHECK(is_valid_int(&object, &result), "Couldn't read integer value for a key");
            switch (field_to_set) {
                case FS_type:
                    data->type = result;
                    break;
                case FS_id:
                    data->id = result;
                    break;
                case FS_thread_id:
                    data->thread_id = result;
                    break;
                case FS_frame_number:
                    data->frame_number = result;
                    break;
                case FS_handle_id:
                    data->handle_id = result;
                    break;
                default:
                    data->parse_fail = 1;
                    data->parse_fail_message = "Field to set NYI";
                    return 0;
            }
            data->fields_set = data->fields_set | field_to_set;
        }
    }

    return check_requirements(data);
}

#define COMMUNICATE_RESULT(operation) do { if((operation)) { communicate_error(&ctx, &argument); } else { communicate_success(&ctx, &argument); } } while (0)

static void debugserver_worker(MVMThreadContext *tc, MVMCallsite *callsite, MVMRegister *args) {
    int continue_running = 1;
    MVMint32 command_serial;
    Socket listensocket;
    MVMInstance *vm = tc->instance;
    MVMuint64 port = vm->debugserver_port;

    vm->debugserver_thread_id = tc->thread_obj->body.thread_id;

    {
        char portstr[16];
        struct addrinfo *res;
        int error;

        snprintf(portstr, 16, "%d", port);

        getaddrinfo("localhost", portstr, NULL, &res);

        listensocket = socket(res->ai_family, SOCK_STREAM, 0);

#ifndef _WIN32
        {
            int one = 1;
            setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        }
#endif

        bind(listensocket, res->ai_addr, res->ai_addrlen);

        freeaddrinfo(res);

        listen(listensocket, 1);
    }

    while(continue_running) {
        Socket clientsocket = accept(listensocket, NULL, NULL);
        int len;
        char *buffer[32];
        cmp_ctx_t ctx;

        send_greeting(&clientsocket);

        if (!receive_greeting(&clientsocket)) {
            fprintf(stderr, "did not receive greeting properly\n");
            close(clientsocket);
            continue;
        }

        cmp_init(&ctx, &clientsocket, socket_reader, NULL, socket_writer);

        while (clientsocket) {
            request_data argument;

            MVM_gc_mark_thread_blocked(tc);
            parse_message_map(&ctx, &argument);
            MVM_gc_mark_thread_unblocked(tc);

            if (argument.parse_fail) {
                fprintf(stderr, "failed to parse this message: %s\n", argument.parse_fail_message);
                cmp_write_map(&ctx, 3);

                cmp_write_str(&ctx, "id", 2);
                cmp_write_integer(&ctx, argument.id);

                cmp_write_str(&ctx, "type", 4);
                cmp_write_integer(&ctx, 1);

                cmp_write_str(&ctx, "reason", 6);
                cmp_write_str(&ctx, argument.parse_fail_message, strlen(argument.parse_fail_message));
                close(clientsocket);
                break;
            }

            fprintf(stderr, "debugserver received packet %d, command %d\n", argument.id, argument.type);

            switch (argument.type) {
                case MT_ResumeAll:
                    COMMUNICATE_RESULT(request_all_threads_resume(tc, &ctx, &argument));
                    break;
                case MT_SuspendOne:
                    COMMUNICATE_RESULT(request_thread_suspends(tc, &ctx, &argument, NULL));
                    break;
                case MT_ResumeOne:
                    COMMUNICATE_RESULT(request_thread_resumes(tc, &ctx, &argument, NULL));
                    break;
                case MT_ThreadListRequest:
                    send_thread_info(tc, &ctx, &argument);
                    break;
                case MT_ThreadStackTraceRequest:
                    if (request_thread_stacktrace(tc, &ctx, &argument, NULL)) {
                        communicate_error(&ctx, &argument);
                    }
                    break;
                case MT_ContextHandle:
                case MT_CodeObjectHandle:
                    if (create_context_or_code_obj_debug_handle(tc, &ctx, &argument, NULL)) {
                        communicate_error(&ctx, &argument);
                    }
                    break;
                case MT_CallerContextRequest:
                    if (create_caller_context_debug_handle(tc, &ctx, &argument, NULL)) {
                        communicate_error(&ctx, &argument);
                    }
                    break;
                case MT_ContextLexicalsRequest:
                    if (request_context_lexicals(tc, &ctx, &argument)) {
                        communicate_error(&ctx, &argument);
                    }
                    break;
                default: /* Unknown command or NYI */
                    fprintf(stderr, "unknown command type (or NYI)\n");
                    cmp_write_map(&ctx, 2);
                    cmp_write_str(&ctx, "id", 2);
                    cmp_write_integer(&ctx, argument.id);
                    cmp_write_str(&ctx, "type", 4);
                    cmp_write_integer(&ctx, 0);
                    break;

            }
        }
    }
}

void MVM_debugserver_init(MVMThreadContext *tc, MVMuint32 port) {
    MVMInstance *vm = tc->instance;
    MVMObject *worker_entry_point;
    int threadCreateError;

    vm->debug_handle_table = MVM_malloc(sizeof(MVMDebugServerHandleTable));

    vm->debug_handle_table->allocated = 32;
    vm->debug_handle_table->used      = 0;
    vm->debug_handle_table->next_id   = 1;
    vm->debug_handle_table->entries   = MVM_calloc(vm->debug_handle_table->allocated, sizeof(MVMDebugServerHandleTableEntry));

    vm->debugserver_port = port;

    worker_entry_point = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTCCode);
    ((MVMCFunction *)worker_entry_point)->body.func = debugserver_worker;
    MVM_thread_run(tc, MVM_thread_new(tc, worker_entry_point, 1));
}

void MVM_debugserver_mark_handles(MVMThreadContext *tc, MVMGCWorklist *worklist, MVMHeapSnapshotState *snapshot) {
    MVMInstance *vm = tc->instance;
    MVMDebugServerHandleTable *table = vm->debug_handle_table;
    MVMuint32 idx;

    for (idx = 0; idx < table->used; idx++) {
        if (worklist)
            MVM_gc_worklist_add(tc, worklist, &(table->entries[idx].target));
        else
            MVM_profile_heap_add_collectable_rel_const_cstr(tc, snapshot,
                (MVMCollectable *)table->entries[idx].target, "Debug Handle");
    }
}