// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkServer.h"
#include "UplinkEditorModule.h"
#include "UplinkCompat.h"
#include "UplinkMcp.h"
#include "UplinkToolRegistry.h"
#include "UplinkTaskManager.h"

#include "Editor.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
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

	TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Request)
	{
		if (Request.Body.Num() == 0)
		{
			return MakeShared<FJsonObject>();
		}
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		const FString BodyString(Converter.Length(), Converter.Get());

		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			return nullptr;
		}
		return Parsed;
	}
}

FUplinkServer::FUplinkServer(FUplinkToolRegistry& InRegistry, FUplinkTaskManager& InTasks)
	: Registry(InRegistry)
	, Tasks(InTasks)
{
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

	// Safety: refuse to start if the engine's HTTP listener has been configured
	// to bind beyond loopback — this server must never be network-reachable.
	FString BindAddress;
	if (GConfig && GConfig->GetString(TEXT("HTTPServer.Listeners"), TEXT("DefaultBindAddress"), BindAddress, GEngineIni))
	{
		if (!BindAddress.IsEmpty() && BindAddress != TEXT("localhost") && BindAddress != TEXT("127.0.0.1"))
		{
			UE_LOG(LogUplink, Error,
				TEXT("Refusing to start: [HTTPServer.Listeners] DefaultBindAddress is '%s' (not loopback). Uplink only serves localhost."),
				*BindAddress);
			return false;
		}
	}

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	Router = HttpServerModule.GetHttpRouter(Port);
	if (!Router.IsValid())
	{
		return false;
	}

	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/status")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FUplinkServer::HandleStatus)));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/tools")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FUplinkServer::HandleListTools)));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/tool")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FUplinkServer::HandleRunTool)));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FUplinkServer::HandleMcp)));

	HttpServerModule.StartAllListeners();
	bRunning = true;

	UE_LOG(LogUplink, Log, TEXT("Uplink listening on http://127.0.0.1:%u  (POST /mcp, GET /status, GET /tools, POST /tool/{name})"), Port);
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
		for (FHttpRouteHandle& Handle : Routes)
		{
			if (Handle.IsValid())
			{
				Router->UnbindRoute(Handle);
			}
		}
	}
	Routes.Empty();
	bRunning = false;
	UE_LOG(LogUplink, Log, TEXT("Uplink server stopped"));
}

bool FUplinkServer::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), TEXT("uplink"));
	Json->SetStringField(TEXT("version"), TEXT("0.2.0"));
	Json->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
	Json->SetStringField(TEXT("project"), FApp::GetProjectName());
	Json->SetBoolField(TEXT("pie_active"), GEditor != nullptr && GEditor->PlayWorld != nullptr);
	Json->SetNumberField(TEXT("tool_count"), Registry.All().Num());

	// Exercise the 5.7/5.8 JSON-key compat shim so a regression fails loudly
	// here, not deep inside a tool.
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
	Json->SetArrayField(TEXT("tools"), Registry.BuildMcpToolList());
	OnComplete(JsonResponse(Json));
	return true;
}

bool FUplinkServer::HandleRunTool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Path is /tool/{name}.
	FString ToolName = Request.RelativePath.GetPath();
	ToolName.RemoveFromStart(TEXT("/tool"));
	ToolName.RemoveFromStart(TEXT("/"));

	const FUplinkToolDef* Def = Registry.Find(ToolName);
	if (!Def)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), false);
		Json->SetStringField(TEXT("message"), FString::Printf(TEXT("unknown tool '%s'"), *ToolName));
		OnComplete(JsonResponse(Json));
		return true;
	}

	TSharedPtr<FJsonObject> Params = ParseBody(Request);
	if (!Params.IsValid())
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), false);
		Json->SetStringField(TEXT("message"), TEXT("request body is not valid JSON"));
		OnComplete(JsonResponse(Json));
		return true;
	}

	FUplinkToolContext Context;
	Context.Params = Params;
	Params->TryGetStringField(FStringView(TEXT("world")), Context.WorldSpec);

	double WaitMs = 25000.0;
	Params->TryGetNumberField(FStringView(TEXT("wait_ms")), WaitMs);
	const double MaxWait = FMath::Clamp(WaitMs / 1000.0, 0.0, 55.0);

	const FGuid TaskId = Tasks.Submit(ToolName, Def->Factory(), MoveTemp(Context), Def->Info.TimeoutSeconds);
	Tasks.Await(TaskId, MaxWait,
		[OnComplete](const FUplinkTask& Task, bool bStillRunning)
		{
			TSharedRef<FJsonObject> Json = UplinkMcp::BuildTaskJson(Task, bStillRunning);
			if (!bStillRunning && Task.Result.Png.Num() > 0)
			{
				Json->SetStringField(TEXT("image_base64"),
					FBase64::Encode(Task.Result.Png.GetData(), static_cast<uint32>(Task.Result.Png.Num())));
				Json->SetStringField(TEXT("image_mime"), TEXT("image/png"));
			}
			OnComplete(JsonResponse(Json));
		});
	return true;
}

bool FUplinkServer::HandleMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return UplinkMcp::Handle(Request, OnComplete, Registry, Tasks);
}
