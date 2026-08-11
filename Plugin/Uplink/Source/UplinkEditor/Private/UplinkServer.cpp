// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkServer.h"
#include "UplinkEditorModule.h"
#include "UplinkCompat.h"

#include "Editor.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString SerializeJson(const TSharedRef<FJsonObject>& Json)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Json, Writer);
		return Out;
	}

	TUniquePtr<FHttpServerResponse> JsonResponse(const TSharedRef<FJsonObject>& Json)
	{
		return FHttpServerResponse::Create(SerializeJson(Json), TEXT("application/json"));
	}
}

FUplinkServer::~FUplinkServer()
{
	Stop();
}

bool FUplinkServer::Start(uint32 InPort)
{
	if (bRunning)
	{
		return true;
	}

	Port = InPort;

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	Router = HttpServerModule.GetHttpRouter(Port);
	if (!Router.IsValid())
	{
		return false;
	}

	StatusHandle = Router->BindRoute(
		FHttpPath(TEXT("/status")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FUplinkServer::HandleStatus));

	ToolsHandle = Router->BindRoute(
		FHttpPath(TEXT("/tools")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FUplinkServer::HandleListTools));

	HttpServerModule.StartAllListeners();
	bRunning = true;

	UE_LOG(LogUplink, Log, TEXT("Uplink listening on http://127.0.0.1:%u  (GET /status, GET /tools)"), Port);
	return true;
}

void FUplinkServer::Stop()
{
	if (!bRunning)
	{
		return;
	}

	if (Router.IsValid())
	{
		if (StatusHandle.IsValid())
		{
			Router->UnbindRoute(StatusHandle);
		}
		if (ToolsHandle.IsValid())
		{
			Router->UnbindRoute(ToolsHandle);
		}
	}

	bRunning = false;
	UE_LOG(LogUplink, Log, TEXT("Uplink server stopped"));
}

bool FUplinkServer::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), TEXT("uplink"));
	Json->SetStringField(TEXT("version"), TEXT("0.1.0"));
	Json->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
	Json->SetStringField(TEXT("project"), FApp::GetProjectName());
	Json->SetBoolField(TEXT("pie_active"), GEditor != nullptr && GEditor->PlayWorld != nullptr);

	// Exercise the 5.7/5.8 JSON-key compat shim so a regression fails loudly here,
	// not deep inside a Phase-1 tool.
	TArray<TSharedPtr<FJsonValue>> KeyNames;
	for (const auto& Pair : Json->Values)
	{
		KeyNames.Add(MakeShared<FJsonValueString>(UplinkCompat::JsonKeyToString(Pair.Key)));
	}
	Json->SetArrayField(TEXT("fields"), KeyNames);

	OnComplete(JsonResponse(Json));
	return true;
}

bool FUplinkServer::HandleListTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("tools"), {});
	OnComplete(JsonResponse(Json));
	return true;
}
