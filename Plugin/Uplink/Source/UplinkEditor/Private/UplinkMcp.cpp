// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkMcp.h"
#include "UplinkEditorModule.h"
#include "UplinkToolRegistry.h"
#include "UplinkTaskManager.h"
#include "UplinkVersion.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	void Respond(const FHttpResultCallback& OnComplete, const TSharedRef<FJsonObject>& Json)
	{
		OnComplete(FHttpServerResponse::Create(UplinkJson::Serialize(Json), TEXT("application/json")));
	}

	TSharedRef<FJsonObject> RpcEnvelope(const TSharedPtr<FJsonValue>& Id)
	{
		TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Envelope->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		return Envelope;
	}

	void RpcResult(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Envelope = RpcEnvelope(Id);
		Envelope->SetObjectField(TEXT("result"), Result);
		Respond(OnComplete, Envelope);
	}

	void RpcError(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		TSharedRef<FJsonObject> Envelope = RpcEnvelope(Id);
		Envelope->SetObjectField(TEXT("error"), Error);
		Respond(OnComplete, Envelope);
	}

	TSharedRef<FJsonObject> TextContent(const FString& Text)
	{
		TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		return Block;
	}
}

UplinkDispatch::FDispatchOutcome UplinkDispatch::Begin(
	FUplinkToolRegistry& Registry,
	FUplinkTaskManager& Tasks,
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params)
{
	FDispatchOutcome Outcome;

	const FUplinkToolDef* Def = Registry.Find(ToolName);
	if (!Def)
	{
		Outcome.RefusalMessage = FString::Printf(TEXT("unknown tool '%s'"), *ToolName);
		return Outcome;
	}

	FUplinkToolContext Context;
	Context.Params = Params;
	if (!Context.Params.IsValid())
	{
		// A tool reads its parameters without checking, so hand it an empty
		// object rather than nothing when a transport had none to give.
		Context.Params = MakeShared<FJsonObject>();
	}
	Context.Params->TryGetStringField(FStringView(TEXT("world")), Context.WorldSpec);

	if (FString ParamError; !FUplinkToolRegistry::ValidateParams(Def->Info, Context.Params, ParamError))
	{
		Outcome.RefusalMessage = ParamError;
		return Outcome;
	}

	// A caller can extend a tool's deadline when a project legitimately needs
	// longer - loading a heavy streaming level, for instance, where the tool
	// failing on time looks like a broken tool rather than a slow map.
	double ToolTimeout = Def->Info.TimeoutSeconds;
	{
		double Override = 0.0;
		if (Context.Params->TryGetNumberField(FStringView(TEXT("timeout_s")), Override) && Override > 0.0)
		{
			ToolTimeout = FMath::Clamp(Override, 1.0, 3600.0);
		}
	}

	// wait_ms bounds how long the caller waits for an answer, not how long the
	// tool may run: past it the reply is a task_id to poll, and the work carries
	// on. The ceiling keeps the response inside a client's own request timeout.
	double WaitMs = 25000.0;
	Context.Params->TryGetNumberField(FStringView(TEXT("wait_ms")), WaitMs);
	const double RequestedWait = FMath::Clamp(WaitMs / 1000.0, 0.0, 55.0);

	// Waiting past the tool's own deadline buys nothing - the task manager has
	// ended it by then - so the shorter of the two wins.
	Outcome.WaitSeconds = FMath::Min(RequestedWait, ToolTimeout + 1.0);

	Outcome.TaskId = Tasks.Submit(
		ToolName, Def->Factory(), MoveTemp(Context), ToolTimeout, Def->Info.bReadOnly, Def->Info.bTransactional);
	Outcome.bAccepted = true;
	return Outcome;
}

TSharedRef<FJsonObject> UplinkMcp::BuildTaskJson(const FUplinkTask& Task, bool bStillRunning)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("task_id"), Task.Id.ToString(EGuidFormats::DigitsWithHyphens));
	Out->SetStringField(TEXT("tool"), Task.ToolName);

	if (bStillRunning)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("status"), TEXT("running"));
		Out->SetStringField(TEXT("message"),
			TEXT("still running - poll with the task_status tool, then fetch task_result"));
		return Out;
	}

	Out->SetBoolField(TEXT("success"), Task.Status == EUplinkTaskStatus::Completed);
	Out->SetStringField(TEXT("status"), FUplinkTaskManager::StatusToString(Task.Status));
	if (!Task.Result.Message.IsEmpty())
	{
		Out->SetStringField(TEXT("message"), Task.Result.Message);
	}
	if (Task.Result.Data.IsValid())
	{
		Out->SetObjectField(TEXT("data"), Task.Result.Data);
	}
	return Out;
}

bool UplinkMcp::Handle(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete,
	FUplinkToolRegistry& Registry,
	FUplinkTaskManager& Tasks)
{
	if (Request.Body.Num() > 2 * 1024 * 1024)
	{
		RpcError(OnComplete, nullptr, -32600, TEXT("request exceeds 2 MB"));
		return true;
	}

	FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	const FString BodyString(Converter.Length(), Converter.Get());

	TSharedPtr<FJsonObject> Root;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			RpcError(OnComplete, nullptr, -32700, TEXT("parse error (JSON-RPC batching is not supported)"));
			return true;
		}
	}

	const TSharedPtr<FJsonValue> Id = Root->TryGetField(FStringView(TEXT("id")));
	FString Method;
	Root->TryGetStringField(FStringView(TEXT("method")), Method);

	// Notifications get 202 Accepted with no body.
	if (Method.StartsWith(TEXT("notifications/")))
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Accepted;
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (Method == TEXT("initialize"))
	{
		FString Requested = TEXT("2025-06-18");
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (Root->TryGetObjectField(FStringView(TEXT("params")), Params) && Params->IsValid())
		{
			(*Params)->TryGetStringField(FStringView(TEXT("protocolVersion")), Requested);
		}
		static const TSet<FString> Supported = {
			TEXT("2025-03-26"), TEXT("2025-06-18"), TEXT("2025-11-25") };

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"),
			Supported.Contains(Requested) ? Requested : TEXT("2025-06-18"));
		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("uplink"));
		ServerInfo->SetStringField(TEXT("version"), UPLINK_VERSION);
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		RpcResult(OnComplete, Id, Result);
		return true;
	}

	if (Method == TEXT("ping"))
	{
		RpcResult(OnComplete, Id, MakeShared<FJsonObject>());
		return true;
	}

	if (Method == TEXT("tools/list"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), Registry.BuildMcpToolList());
		RpcResult(OnComplete, Id, Result);
		return true;
	}

	if (Method == TEXT("tools/call"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Root->TryGetObjectField(FStringView(TEXT("params")), Params) || !Params->IsValid())
		{
			RpcError(OnComplete, Id, -32602, TEXT("missing params"));
			return true;
		}
		FString ToolName;
		(*Params)->TryGetStringField(FStringView(TEXT("name")), ToolName);

		TSharedPtr<FJsonObject> Arguments;
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if ((*Params)->TryGetObjectField(FStringView(TEXT("arguments")), Found) && Found->IsValid())
		{
			Arguments = *Found;
		}
		else
		{
			Arguments = MakeShared<FJsonObject>();
		}

		const UplinkDispatch::FDispatchOutcome Outcome =
			UplinkDispatch::Begin(Registry, Tasks, ToolName, Arguments);
		if (!Outcome.bAccepted)
		{
			RpcError(OnComplete, Id, -32602, Outcome.RefusalMessage);
			return true;
		}

		// Hold the HTTP response until the task finishes or the wait budget runs
		// out; never blocks the game thread (the waiter fires from the task ticker).
		Tasks.Await(Outcome.TaskId, Outcome.WaitSeconds,
			[OnComplete, Id](const FUplinkTask& Task, bool bStillRunning)
			{
				TArray<TSharedPtr<FJsonValue>> Content;
				if (!bStillRunning && Task.Result.Png.Num() > 0)
				{
					TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
					Image->SetStringField(TEXT("type"), TEXT("image"));
					Image->SetStringField(TEXT("data"),
						FBase64::Encode(Task.Result.Png.GetData(), static_cast<uint32>(Task.Result.Png.Num())));
					Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
					Content.Add(MakeShared<FJsonValueObject>(Image));
				}
				Content.Add(MakeShared<FJsonValueObject>(
					TextContent(UplinkJson::Serialize(UplinkMcp::BuildTaskJson(Task, bStillRunning)))));

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetArrayField(TEXT("content"), Content);
				if (!bStillRunning && Task.Status != EUplinkTaskStatus::Completed)
				{
					Result->SetBoolField(TEXT("isError"), true);
				}
				RpcResult(OnComplete, Id, Result);
			});
		return true;
	}

	RpcError(OnComplete, Id, -32601, FString::Printf(TEXT("method not found: %s"), *Method));
	return true;
}
