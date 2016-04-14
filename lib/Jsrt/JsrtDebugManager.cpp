//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include "JsrtPch.h"
#include "JsrtDebugManager.h"
#include "JsrtDebugEventObject.h"
#include "JsrtDebugUtils.h"
#include "JsrtDebuggerObject.h"
#include "RuntimeDebugPch.h"
#include "screrror.h"   // For CompileScriptException

JsrtDebugManager::JsrtDebugManager(ThreadContext* threadContext) :
    HostDebugContext(nullptr),
    threadContext(threadContext),
    debugEventCallback(nullptr),
    callbackState(nullptr),
    resumeAction(BREAKRESUMEACTION_CONTINUE),
    debugObjectArena(nullptr),
    debuggerObjectsManager(nullptr),
    callBackDepth(0),
    debugDocumentManager(nullptr),
    stackFrames(nullptr),
    breakOnExceptionAttributes(JsDiagBreakOnExceptionAttributeUncaught)
{
    Assert(threadContext != nullptr);
}

JsrtDebugManager::~JsrtDebugManager()
{
    if (this->debuggerObjectsManager != nullptr)
    {
        Adelete(this->debugObjectArena, this->debuggerObjectsManager);
        this->debuggerObjectsManager = nullptr;
    }

    if (this->debugDocumentManager != nullptr)
    {
        Adelete(this->debugObjectArena, this->debugDocumentManager);
        this->debugDocumentManager = nullptr;
    }

    if (this->debugObjectArena != nullptr)
    {
        this->threadContext->GetRecycler()->UnregisterExternalGuestArena(this->debugObjectArena);
        HeapDelete(this->debugObjectArena);
        this->debugObjectArena = nullptr;
    }

    this->debugEventCallback = nullptr;
    this->callbackState = nullptr;
    this->threadContext = nullptr;
}

void JsrtDebugManager::SetDebugEventCallback(JsDiagDebugEventCallback debugEventCallback, void* callbackState)
{
    Assert(this->debugEventCallback == nullptr);
    Assert(this->callbackState == nullptr);

    this->debugEventCallback = debugEventCallback;
    this->callbackState = callbackState;
}

void* JsrtDebugManager::GetAndClearCallbackState()
{
    void* currentCallbackState = this->callbackState;

    this->debugEventCallback = nullptr;
    this->callbackState = nullptr;

    return currentCallbackState;
}

bool JsrtDebugManager::IsDebugEventCallbackSet() const
{
    return this->debugEventCallback != nullptr;
}

bool JsrtDebugManager::CanHalt(Js::InterpreterHaltState* haltState)
{
    // This is registered as the callback for inline breakpoints.
    // We decide here if we are at a reasonable stop location that has source code.
    Assert(haltState->IsValid());

    Js::FunctionBody* pCurrentFuncBody = haltState->GetFunction();
    int byteOffset = haltState->GetCurrentOffset();
    Js::FunctionBody::StatementMap* map = pCurrentFuncBody->GetMatchingStatementMapFromByteCode(byteOffset, false);

    // Resolve the dummy ret code.
    return map != nullptr && (!pCurrentFuncBody->GetIsGlobalFunc() || !Js::FunctionBody::IsDummyGlobalRetStatement(&map->sourceSpan));
}

void JsrtDebugManager::DispatchHalt(Js::InterpreterHaltState* haltState)
{
    switch (haltState->stopType)
    {
    case Js::STOP_BREAKPOINT: /*JsDiagDebugEventBreak*/
    case Js::STOP_INLINEBREAKPOINT: /*JsDiagDebugEventDebuggerStatement*/
    case Js::STOP_ASYNCBREAK: /*JsDiagDebugEventAsyncBreak*/
        this->ReportBreak(haltState);
        break;
    case Js::STOP_STEPCOMPLETE: /*JsDiagDebugEventStepComplete*/
        this->SetResumeType(BREAKRESUMEACTION_CONTINUE);
        this->ReportBreak(haltState);
        break;
    case Js::STOP_EXCEPTIONTHROW: /*JsDiagDebugEventRuntimeException*/
        this->ReportExceptionBreak(haltState);
        break;
    case Js::STOP_MUTATIONBREAKPOINT:
        AssertMsg(false, "Not yet handled");
        break;
    default:
        AssertMsg(false, "Unhandled stop type");
    }

    this->HandleResume(haltState, this->resumeAction);
}

bool JsrtDebugManager::CanAllowBreakpoints()
{
    return true;
}

void JsrtDebugManager::CleanupHalt()
{
}

bool JsrtDebugManager::IsInClosedState()
{
    return this->debugEventCallback == nullptr;
}

bool JsrtDebugManager::IsExceptionReportingEnabled()
{
    return this->GetBreakOnException() != JsDiagBreakOnExceptionAttributeNone;
}

bool JsrtDebugManager::IsFirstChanceExceptionEnabled()
{
    return (this->GetBreakOnException() & JsDiagBreakOnExceptionAttributeFirstChance) == JsDiagBreakOnExceptionAttributeFirstChance;
}

HRESULT JsrtDebugManager::DbgRegisterFunction(Js::ScriptContext* scriptContext, Js::FunctionBody* functionBody, DWORD_PTR dwDebugSourceContext, LPCWSTR title)
{
    Js::Utf8SourceInfo* utf8SourceInfo = functionBody->GetUtf8SourceInfo();

    if (!utf8SourceInfo->GetIsLibraryCode() && !utf8SourceInfo->HasDebugDocument())
    {
        JsrtDebugDocumentManager* debugDocumentManager = this->GetDebugDocumentManager();
        Assert(debugDocumentManager != nullptr);

        Js::DebugDocument* debugDocument = HeapNewNoThrow(Js::DebugDocument, utf8SourceInfo, functionBody);
        if (debugDocument != nullptr)
        {
            utf8SourceInfo->SetDebugDocument(debugDocument);
        }
    }

    return S_OK;
}

void JsrtDebugManager::ReportScriptCompile(Js::JavascriptFunction* scriptFunction, Js::Utf8SourceInfo* utf8SourceInfo, CompileScriptException* compileException)
{
    if (this->debugEventCallback != nullptr)
    {
        Js::ScriptContext* scriptContext = utf8SourceInfo->GetScriptContext();

        JsrtDebugEventObject debugEventObject(scriptContext);

        Js::DynamicObject* eventDataObject = debugEventObject.GetEventDataObject();

        JsrtDebugUtils::AddFileNameOrScriptTypeToObject(eventDataObject, utf8SourceInfo);
        JsrtDebugUtils::AddLineCountToObject(eventDataObject, utf8SourceInfo);
        JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::sourceLength, utf8SourceInfo->GetCchLength(), utf8SourceInfo->GetScriptContext());

        JsDiagDebugEvent jsDiagDebugEvent = JsDiagDebugEventCompileError;

        if (scriptFunction == nullptr)
        {
            // Report JsDiagDebugEventCompileError event
            JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::error, compileException->ei.bstrDescription, scriptContext);
            JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::line, compileException->line, scriptContext);
            JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::column, compileException->ichMin - compileException->ichMinLine - 1, scriptContext); // Converted to 0-based
            JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::sourceText, compileException->bstrLine, scriptContext);
        }
        else
        {
            JsrtDebugDocumentManager* debugDocumentManager = this->GetDebugDocumentManager();
            Assert(debugDocumentManager != nullptr);

            // Create DebugDocument and then report JsDiagDebugEventSourceCompile event
            Js::DebugDocument* debugDocument = HeapNewNoThrow(Js::DebugDocument, utf8SourceInfo, scriptFunction->GetFunctionBody());
            if (debugDocument != nullptr)
            {
                utf8SourceInfo->SetDebugDocument(debugDocument);

                // Only add scriptId if everything is ok as scriptId is used for other operations
                JsrtDebugUtils::AddScriptIdToObject(eventDataObject, utf8SourceInfo);
            }
            jsDiagDebugEvent = JsDiagDebugEventSourceCompile;
        }

        this->CallDebugEventCallback(jsDiagDebugEvent, eventDataObject, scriptContext);
    }
}

void JsrtDebugManager::ReportBreak(Js::InterpreterHaltState* haltState)
{
    if (this->debugEventCallback != nullptr)
    {
        Js::FunctionBody* functionBody = haltState->GetFunction();
        Assert(functionBody != nullptr);

        Js::Utf8SourceInfo* utf8SourceInfo = functionBody->GetUtf8SourceInfo();
        int currentByteCodeOffset = haltState->GetCurrentOffset();
        Js::ScriptContext* scriptContext = utf8SourceInfo->GetScriptContext();

        JsDiagDebugEvent jsDiagDebugEvent = this->GetDebugEventFromStopType(haltState->stopType);

        JsrtDebugEventObject debugEventObject(scriptContext);

        Js::DynamicObject* eventDataObject = debugEventObject.GetEventDataObject();

        Js::ProbeContainer* probeContainer = scriptContext->GetDebugContext()->GetProbeContainer();

        if (jsDiagDebugEvent == JsDiagDebugEventBreak)
        {
            UINT bpId = 0;
            probeContainer->MapProbesUntil([&](int i, Js::Probe* pProbe)
            {
                Js::BreakpointProbe* bp = (Js::BreakpointProbe*)pProbe;
                if (bp->Matches(functionBody, utf8SourceInfo->GetDebugDocument(), currentByteCodeOffset))
                {
                    bpId = bp->GetId();
                    return true;
                }
                return false;
            });

            AssertMsg(bpId != 0, "How come we don't have a breakpoint id for JsDiagDebugEventBreak");

            JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::breakpointId, bpId, scriptContext);
        }

        JsrtDebugUtils::AddScriptIdToObject(eventDataObject, utf8SourceInfo);
        JsrtDebugUtils::AddLineColumnToObject(eventDataObject, functionBody, currentByteCodeOffset);
        JsrtDebugUtils::AddSourceLengthAndTextToObject(eventDataObject, functionBody, currentByteCodeOffset);

        this->CallDebugEventCallbackForBreak(jsDiagDebugEvent, eventDataObject, scriptContext);
    }
}

void JsrtDebugManager::ReportExceptionBreak(Js::InterpreterHaltState* haltState)
{
    if (this->debugEventCallback != nullptr)
    {
        Assert(haltState->stopType == Js::STOP_EXCEPTIONTHROW);

        Js::Utf8SourceInfo* utf8SourceInfo = haltState->GetFunction()->GetUtf8SourceInfo();
        Js::ScriptContext* scriptContext = utf8SourceInfo->GetScriptContext();

        JsDiagDebugEvent jsDiagDebugEvent = JsDiagDebugEventRuntimeException;

        JsrtDebugEventObject debugEventObject(scriptContext);

        Js::DynamicObject* eventDataObject = debugEventObject.GetEventDataObject();

        JsrtDebugUtils::AddScriptIdToObject(eventDataObject, utf8SourceInfo);

        Js::FunctionBody* functionBody = haltState->topFrame->GetFunction();

        Assert(functionBody != nullptr);
        
        int currentByteCodeOffset = haltState->topFrame->GetByteCodeOffset();
        JsrtDebugUtils::AddLineColumnToObject(eventDataObject, functionBody, currentByteCodeOffset);
        JsrtDebugUtils::AddSourceLengthAndTextToObject(eventDataObject, functionBody, currentByteCodeOffset);
        JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::uncaught, !haltState->exceptionObject->IsFirstChanceException(), scriptContext);

        Js::ResolvedObject resolvedObject;
        resolvedObject.scriptContext = scriptContext;
        resolvedObject.name = _u("{exception}");
        resolvedObject.typeId = Js::TypeIds_Error;
        resolvedObject.address = nullptr;
        resolvedObject.obj = scriptContext->GetDebugContext()->GetProbeContainer()->GetExceptionObject();

        if (resolvedObject.obj == nullptr)
        {
            resolvedObject.obj = resolvedObject.scriptContext->GetLibrary()->GetUndefined();
        }

        JsrtDebuggerObjectBase::CreateDebuggerObject<JsrtDebuggerObjectProperty>(this->GetDebuggerObjectsManager(), resolvedObject, scriptContext, [&](Js::Var marshaledObj)
        {
            JsrtDebugUtils::AddPropertyToObject(eventDataObject, JsrtDebugPropertyId::exception, marshaledObj, scriptContext);
        });
        
        this->CallDebugEventCallbackForBreak(jsDiagDebugEvent, eventDataObject, scriptContext);
    }
}

void JsrtDebugManager::HandleResume(Js::InterpreterHaltState* haltState, BREAKRESUMEACTION resumeAction)
{
    Js::ScriptContext* scriptContext = haltState->framePointers->Peek()->GetScriptContext();

    if (resumeAction == BREAKRESUMEACTION_ABORT)
    {
        // In this case we need to abort script execution entirely and also stop working on any breakpoint for this engine.
        scriptContext->GetDebugContext()->GetProbeContainer()->RemoveAllProbes();
        scriptContext->GetDebugContext()->GetProbeContainer()->UninstallInlineBreakpointProbe(NULL);
        scriptContext->GetDebugContext()->GetProbeContainer()->UninstallDebuggerScriptOptionCallback();
        throw Js::ScriptAbortException();
    }
    else
    {
        scriptContext->GetThreadContext()->GetDebugManager()->stepController.HandleResumeAction(haltState, resumeAction);
    }
}

void JsrtDebugManager::SetResumeType(BREAKRESUMEACTION resumeAction)
{
    this->resumeAction = resumeAction;
}

bool JsrtDebugManager::EnableAsyncBreak(Js::ScriptContext* scriptContext)
{
    // This can be called when we are already at break
    if (!scriptContext->GetDebugContext()->GetProbeContainer()->IsAsyncActivate())
    {
        scriptContext->GetDebugContext()->GetProbeContainer()->AsyncActivate(this);
        if (Js::Configuration::Global.EnableJitInDebugMode())
        {
            scriptContext->GetThreadContext()->GetDebugManager()->GetDebuggingFlags()->SetForceInterpreter(true);
        }
        return true;
    }
    return false;
}

void JsrtDebugManager::CallDebugEventCallback(JsDiagDebugEvent debugEvent, Js::DynamicObject* eventDataObject, Js::ScriptContext* scriptContext)
{
    class AutoClear
    {
    public:
        AutoClear(JsrtDebugManager* jsrtDebug)
        {
            this->jsrtDebugManager = jsrtDebug;
            jsrtDebugManager->callBackDepth++;
        }

        ~AutoClear()
        {
            jsrtDebugManager->callBackDepth--;

            if (jsrtDebugManager->callBackDepth == 0)
            {
                if (jsrtDebugManager->debuggerObjectsManager != nullptr)
                {
                    jsrtDebugManager->GetDebuggerObjectsManager()->ClearAll();
                }

                if (jsrtDebugManager->stackFrames != nullptr)
                {
                    Adelete(jsrtDebugManager->GetDebugObjectArena(), jsrtDebugManager->stackFrames);
                    jsrtDebugManager->stackFrames = nullptr;
                }
            }

            this->jsrtDebugManager = nullptr;
        }
    private:
        JsrtDebugManager* jsrtDebugManager;
    };

    auto funcPtr = [&]()
    {
        AutoClear autoClear(this);
        this->debugEventCallback(debugEvent, eventDataObject, this->callbackState);
    };

    if (scriptContext->GetThreadContext()->IsScriptActive())
    {
        BEGIN_LEAVE_SCRIPT(scriptContext)
        {
            funcPtr();
        }
        END_LEAVE_SCRIPT(scriptContext);
    }
    else
    {
        funcPtr();
    }
}

void JsrtDebugManager::CallDebugEventCallbackForBreak(JsDiagDebugEvent debugEvent, Js::DynamicObject* eventDataObject, Js::ScriptContext* scriptContext)
{
    AutoSetDispatchHaltFlag autoSetDispatchHaltFlag(scriptContext, scriptContext->GetThreadContext());

#if DBG
    void *frameAddress = _AddressOfReturnAddress();
    scriptContext->GetThreadContext()->GetDebugManager()->SetDispatchHaltFrameAddress(frameAddress);
#endif

    this->CallDebugEventCallback(debugEvent, eventDataObject, scriptContext);

    for (Js::ScriptContext *tempScriptContext = scriptContext->GetThreadContext()->GetScriptContextList();
    tempScriptContext != nullptr && !tempScriptContext->IsClosed();
        tempScriptContext = tempScriptContext->next)
    {
        tempScriptContext->GetDebugContext()->GetProbeContainer()->AsyncDeactivate();
    }

    if (Js::Configuration::Global.EnableJitInDebugMode())
    {
        scriptContext->GetThreadContext()->GetDebugManager()->GetDebuggingFlags()->SetForceInterpreter(false);
    }
}

Js::DynamicObject* JsrtDebugManager::GetScript(Js::Utf8SourceInfo* utf8SourceInfo)
{
    Js::DynamicObject* scriptObject = utf8SourceInfo->GetScriptContext()->GetLibrary()->CreateObject();

    JsrtDebugUtils::AddScriptIdToObject(scriptObject, utf8SourceInfo);
    JsrtDebugUtils::AddFileNameOrScriptTypeToObject(scriptObject, utf8SourceInfo);
    JsrtDebugUtils::AddLineCountToObject(scriptObject, utf8SourceInfo);
    JsrtDebugUtils::AddPropertyToObject(scriptObject, JsrtDebugPropertyId::sourceLength, utf8SourceInfo->GetCchLength(), utf8SourceInfo->GetScriptContext());

    return scriptObject;
}

Js::JavascriptArray* JsrtDebugManager::GetScripts(Js::ScriptContext* scriptContext)
{
    Js::JavascriptArray* scriptsArray = scriptContext->GetLibrary()->CreateArray();

    int index = 0;

    for (Js::ScriptContext *tempScriptContext = scriptContext->GetThreadContext()->GetScriptContextList();
    tempScriptContext != nullptr && !tempScriptContext->IsClosed();
        tempScriptContext = tempScriptContext->next)
    {
        tempScriptContext->GetSourceList()->Map([&](int i, RecyclerWeakReference<Js::Utf8SourceInfo>* utf8SourceInfoWeakRef)
        {
            Js::Utf8SourceInfo* utf8SourceInfo = utf8SourceInfoWeakRef->Get();
            if (utf8SourceInfo != nullptr && !utf8SourceInfo->GetIsLibraryCode() && utf8SourceInfo->HasDebugDocument())
            {
                bool isCallerLibraryCode = false;

                bool isDynamic = utf8SourceInfo->IsDynamic();

                if (isDynamic)
                {
                    // If the code is dynamic (eval or new Function) only return the script if parent is non-library
                    Js::Utf8SourceInfo* callerUtf8SourceInfo = utf8SourceInfo->GetCallerUtf8SourceInfo();

                    while (callerUtf8SourceInfo != nullptr && !isCallerLibraryCode)
                    {
                        isCallerLibraryCode = callerUtf8SourceInfo->GetIsLibraryCode();
                        callerUtf8SourceInfo = callerUtf8SourceInfo->GetCallerUtf8SourceInfo();
                    }
                }

                if (!isCallerLibraryCode)
                {
                    Js::DynamicObject* sourceObj = this->GetScript(utf8SourceInfo);

                    if (sourceObj != nullptr)
                    {
                        Js::Var marshaledObj = Js::CrossSite::MarshalVar(scriptContext, sourceObj);
                        Js::JavascriptOperators::OP_SetElementI((Js::Var)scriptsArray, Js::JavascriptNumber::ToVar(index, scriptContext), marshaledObj, scriptContext);
                        index++;
                    }
                }
            }
        });
    }

    return scriptsArray;
}

Js::DynamicObject* JsrtDebugManager::GetSource(uint scriptId)
{
    Js::Utf8SourceInfo* utf8SourceInfo = nullptr;

    for (Js::ScriptContext *scriptContext = this->threadContext->GetScriptContextList();
    scriptContext != nullptr && utf8SourceInfo == nullptr && !scriptContext->IsClosed();
        scriptContext = scriptContext->next)
    {
        scriptContext->GetSourceList()->MapUntil([&](int i, RecyclerWeakReference<Js::Utf8SourceInfo>* sourceInfoWeakRef) -> bool
        {
            Js::Utf8SourceInfo* sourceInfo = sourceInfoWeakRef->Get();
            if (sourceInfo != nullptr && sourceInfo->IsInDebugMode() && sourceInfo->GetSourceInfoId() == scriptId)
            {
                utf8SourceInfo = sourceInfo;
                return true;
            }
            return false;
        });
    }

    Js::DynamicObject* sourceObject = nullptr;

    if (utf8SourceInfo != nullptr)
    {
        sourceObject = utf8SourceInfo->GetScriptContext()->GetLibrary()->CreateObject();

        JsrtDebugUtils::AddScriptIdToObject(sourceObject, utf8SourceInfo);
        JsrtDebugUtils::AddFileNameOrScriptTypeToObject(sourceObject, utf8SourceInfo);
        JsrtDebugUtils::AddLineCountToObject(sourceObject, utf8SourceInfo);
        JsrtDebugUtils::AddPropertyToObject(sourceObject, JsrtDebugPropertyId::sourceLength, utf8SourceInfo->GetCchLength(), utf8SourceInfo->GetScriptContext());
        JsrtDebugUtils::AddSouceToObject(sourceObject, utf8SourceInfo);
    }

    return sourceObject;
}

Js::JavascriptArray* JsrtDebugManager::GetStackFrames(Js::ScriptContext* scriptContext)
{
    if (this->stackFrames != nullptr)
    {
        return this->stackFrames->StackFrames(scriptContext);
    }

    this->stackFrames = Anew(this->GetDebugObjectArena(), JsrtDebugStackFrames, this);

    return this->stackFrames->StackFrames(scriptContext);
}

bool JsrtDebugManager::TryGetFrameObjectFromFrameIndex(Js::ScriptContext *scriptContext, uint frameIndex, JsrtDebuggerStackFrame ** debuggerStackFrame)
{
    if (this->stackFrames == nullptr)
    {
        this->GetStackFrames(scriptContext);
    }

    Assert(this->stackFrames != nullptr);

    return this->stackFrames->TryGetFrameObjectFromFrameIndex(frameIndex, debuggerStackFrame);
}

Js::DynamicObject* JsrtDebugManager::SetBreakPoint(Js::Utf8SourceInfo* utf8SourceInfo, UINT lineNumber, UINT columnNumber)
{
    Js::DebugDocument* debugDocument = utf8SourceInfo->GetDebugDocument();
    if (debugDocument != nullptr && SUCCEEDED(utf8SourceInfo->EnsureLineOffsetCacheNoThrow()) && lineNumber < utf8SourceInfo->GetLineCount())
    {
        charcount_t charPosition;
        charcount_t byteOffset;
        utf8SourceInfo->GetCharPositionForLineInfo((charcount_t)lineNumber, &charPosition, &byteOffset);
        long ibos = charPosition + columnNumber + 1;

        Js::StatementLocation statement;
        if (!debugDocument->GetStatementLocation(ibos, &statement))
        {
            return nullptr;
        }

        // Don't see a use case for supporting multiple breakpoints at same location.
        // If a breakpoint already exists, just return that
        Js::BreakpointProbe* probe = debugDocument->FindBreakpointId(statement);
        if (probe == nullptr)
        {
            probe = debugDocument->SetBreakPoint(statement, BREAKPOINT_ENABLED);

            if(probe == nullptr)
            {
                return nullptr;
            }

            this->GetDebugDocumentManager()->AddDocument(probe->GetId(), debugDocument);
        }

        probe->GetStatementLocation(&statement);

        Js::ScriptContext* scriptContext = debugDocument->GetUtf8SourceInfo()->GetScriptContext();

        Js::DynamicObject* bpObject = scriptContext->GetLibrary()->CreateObject();

        JsrtDebugUtils::AddPropertyToObject(bpObject, JsrtDebugPropertyId::breakpointId, probe->GetId(), scriptContext);
        JsrtDebugUtils::AddLineColumnToObject(bpObject, statement.function, statement.bytecodeSpan.begin);
        JsrtDebugUtils::AddScriptIdToObject(bpObject, utf8SourceInfo);

        return bpObject;
    }

    return nullptr;
}

void JsrtDebugManager::GetBreakpoints(Js::JavascriptArray** bpsArray, Js::ScriptContext* scriptContext)
{
    Js::ScriptContext* arrayScriptContext = (*bpsArray)->GetScriptContext();
    Js::ProbeContainer* probeContainer = scriptContext->GetDebugContext()->GetProbeContainer();

    probeContainer->MapProbes([&](int i, Js::Probe* pProbe)
    {

        Js::BreakpointProbe* bp = (Js::BreakpointProbe*)pProbe;
        Js::DynamicObject* bpObject = scriptContext->GetLibrary()->CreateObject();

        JsrtDebugUtils::AddPropertyToObject(bpObject, JsrtDebugPropertyId::breakpointId, bp->GetId(), scriptContext);
        JsrtDebugUtils::AddLineColumnToObject(bpObject, bp->GetFunctionBody(), bp->GetBytecodeOffset());

        Js::Utf8SourceInfo* utf8SourceInfo = bp->GetDbugDocument()->GetUtf8SourceInfo();
        JsrtDebugUtils::AddScriptIdToObject(bpObject, utf8SourceInfo);

        Js::Var marshaledObj = Js::CrossSite::MarshalVar(arrayScriptContext, bpObject);
        Js::JavascriptOperators::OP_SetElementI((Js::Var)(*bpsArray), Js::JavascriptNumber::ToVar((*bpsArray)->GetLength(), arrayScriptContext), marshaledObj, arrayScriptContext);
    });
}

JsrtDebuggerObjectsManager* JsrtDebugManager::GetDebuggerObjectsManager()
{
    if (this->debuggerObjectsManager == nullptr)
    {
        this->debuggerObjectsManager = Anew(this->GetDebugObjectArena(), JsrtDebuggerObjectsManager, this);
    }
    return this->debuggerObjectsManager;
}

void JsrtDebugManager::ClearDebuggerObjects()
{
    if (this->debuggerObjectsManager != nullptr)
    {
        this->debuggerObjectsManager->ClearAll();
    }
}

ArenaAllocator* JsrtDebugManager::GetDebugObjectArena()
{
    if (this->debugObjectArena == nullptr)
    {
        this->debugObjectArena = HeapNew(ArenaAllocator, _u("DebugObjectArena"), this->threadContext->GetPageAllocator(), Js::Throw::OutOfMemory);

        this->threadContext->GetRecycler()->RegisterExternalGuestArena(this->debugObjectArena);
    }

    return this->debugObjectArena;
}

JsrtDebugDocumentManager* JsrtDebugManager::GetDebugDocumentManager()
{
    if (this->debugDocumentManager == nullptr)
    {
        this->debugDocumentManager = Anew(this->GetDebugObjectArena(), JsrtDebugDocumentManager, this);
    }
    return this->debugDocumentManager;
}

void JsrtDebugManager::ClearDebugDocument(Js::ScriptContext* scriptContext)
{
    if (this->debugDocumentManager != nullptr)
    {
        this->debugDocumentManager->ClearDebugDocument(scriptContext);
    }
}

void JsrtDebugManager::ClearBreakpointDebugDocumentDictionary()
{
    if (this->debugDocumentManager != nullptr)
    {
        this->debugDocumentManager->ClearBreakpointDebugDocumentDictionary();
    }
}

bool JsrtDebugManager::RemoveBreakpoint(UINT breakpointId)
{
    if (this->debugDocumentManager != nullptr)
    {
        return this->GetDebugDocumentManager()->RemoveBreakpoint(breakpointId);
    }

    return false;
}

void JsrtDebugManager::SetBreakOnException(JsDiagBreakOnExceptionAttributes exceptionAttributes)
{
    this->breakOnExceptionAttributes = exceptionAttributes;
}

JsDiagBreakOnExceptionAttributes JsrtDebugManager::GetBreakOnException()
{
    return this->breakOnExceptionAttributes;
}

JsDiagDebugEvent JsrtDebugManager::GetDebugEventFromStopType(Js::StopType stopType)
{
    switch (stopType)
    {
    case Js::STOP_BREAKPOINT: return JsDiagDebugEventBreak;
    case Js::STOP_INLINEBREAKPOINT: return JsDiagDebugEventDebuggerStatement;
    case Js::STOP_STEPCOMPLETE: return JsDiagDebugEventStepComplete;
    case Js::STOP_EXCEPTIONTHROW: return JsDiagDebugEventRuntimeException;
    case Js::STOP_ASYNCBREAK: return JsDiagDebugEventAsyncBreak;

    case Js::STOP_MUTATIONBREAKPOINT:
    default:
        Assert("Unhandled stoptype");
        break;
    }

    return JsDiagDebugEventBreak;
}