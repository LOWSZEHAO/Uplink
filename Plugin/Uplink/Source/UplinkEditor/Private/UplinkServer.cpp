// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkServer.h"
#include "UplinkEditorModule.h"
#include "UplinkCompat.h"
#include "UplinkMcp.h"
#include "UplinkToolRegistry.h"
#include "UplinkTaskManager.h"
#include "UplinkVersion.h"

#include "Editor.h"
#include "HAL/PlatformMisc.h"
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

	constexpr int32 MaxRequestBodyBytes = 2 * 1024 * 1024;

	TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Request)
	{
		if (Request.Body.Num() == 0)
		{
			return MakeShared<FJsonObject>();
		}
		if (Request.Body.Num() > MaxRequestBodyBytes)
		{
			return nullptr;
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

	// A refused REST call still answers 200 with success=false: the body is the
	// error, and a client reads it exactly the way it reads a tool's own failure.
	void RefuseRest(const FHttpResultCallback& OnComplete, const FString& Message)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), false);
		Json->SetStringField(TEXT("message"), Message);
		OnComplete(JsonResponse(Json));
	}

	/**
	 * An Origin header is a serialised origin - "scheme://host" with an optional
	 * ":port" - and nothing else, so take it apart and compare the pieces.
	 *
	 * Matching the prefix "http://localhost" instead accepts
	 * http://localhost.evil.com, which is a page on somebody else's server
	 * talking to this editor through the user's browser. That is the one attack
	 * a loopback-only server is still exposed to, so the host has to match
	 * whole, not from the front.
	 */
	bool IsLoopbackOrigin(const FString& Origin)
	{
		FString Remainder = Origin;
		Remainder.TrimStartAndEndInline();

		const int32 SchemeEnd = Remainder.Find(TEXT("://"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (SchemeEnd == INDEX_NONE)
		{
			return false;
		}
		const FString Scheme = Remainder.Left(SchemeEnd).ToLower();
		if (Scheme != TEXT("http") && Scheme != TEXT("https"))
		{
			return false;
		}

		FString Authority = Remainder.Mid(SchemeEnd + 3);

		// A browser never puts a path in an Origin, but a hand-written one often
		// ends in a slash. Allow exactly that and nothing further.
		int32 SlashIndex = INDEX_NONE;
		if (Authority.FindChar(TCHAR('/'), SlashIndex))
		{
			if (Authority.Len() != SlashIndex + 1)
			{
				return false;
			}
			Authority.LeftInline(SlashIndex);
		}

		// "user@host" would make the host the part after the '@', which is not
		// how any of this is being read below. It is never a real origin either.
		if (Authority.IsEmpty() || Authority.Contains(TEXT("@")))
		{
			return false;
		}

		// An unbracketed "::1" cannot carry a port - nothing separates the
		// address's colons from the port's - so it is only ever the whole host.
		if (Authority.Equals(TEXT("::1")))
		{
			return true;
		}

		FString Host = Authority;
		FString PortPart;
		if (Host.StartsWith(TEXT("[")))
		{
			// Bracketed IPv6 literal: any port follows the closing bracket.
			int32 CloseIndex = INDEX_NONE;
			if (!Host.FindChar(TCHAR(']'), CloseIndex))
			{
				return false;
			}
			PortPart = Host.Mid(CloseIndex + 1);
			Host.LeftInline(CloseIndex + 1);
		}
		else
		{
			int32 ColonIndex = INDEX_NONE;
			if (Host.FindChar(TCHAR(':'), ColonIndex))
			{
				PortPart = Host.Mid(ColonIndex);
				Host.LeftInline(ColonIndex);
			}
		}

		// Whatever the port is, it has to be a port. Digits after one colon is
		// the only shape that is, and that rules out a second address hiding here.
		if (!PortPart.IsEmpty())
		{
			if (PortPart.Len() < 2 || PortPart[0] != TCHAR(':'))
			{
				return false;
			}
			for (int32 Index = 1; Index < PortPart.Len(); ++Index)
			{
				if (!FChar::IsDigit(PortPart[Index]))
				{
					return false;
				}
			}
		}

		Host.ToLowerInline();
		return Host == TEXT("localhost") || Host == TEXT("127.0.0.1") || Host == TEXT("[::1]");
	}

	// Browser-CSRF guard: non-browser clients send no Origin header and pass;
	// anything claiming an origin that is not loopback is denied.
	//
	// The key is lowercase because the engine lowercases every header name while
	// parsing (HttpConnectionRequestReadContext). Case is not load-bearing here -
	// FString map keys hash and compare case-insensitively, so "Origin" would find
	// it too - but match what the map actually holds.
	bool IsOriginAllowed(const FHttpServerRequest& Request)
	{
		const TArray<FString>* Origins = Request.Headers.Find(TEXT("origin"));
		if (!Origins)
		{
			return true;
		}
		for (const FString& Origin : *Origins)
		{
			if (!IsLoopbackOrigin(Origin))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Compare a presented secret against the expected one in time that does not
	 * depend on how much of it was right.
	 *
	 * FString's own == stops at the first character that differs, and the time
	 * that saves is measurable across enough requests: it turns guessing a token
	 * into guessing one character at a time. Reading both strings to the end and
	 * folding every difference together costs the same whatever arrives. It is
	 * also case-sensitive, which == is not, and a token has to be.
	 */
	bool SecretsMatch(const FString& Presented, const FString& Expected)
	{
		// Length is not the secret - the caller already knows what it sent - but
		// a length mismatch still has to cost the same as any other, so record it
		// and keep going rather than returning early.
		uint32 Difference = static_cast<uint32>(Presented.Len() ^ Expected.Len());

		const int32 Count = FMath::Max(Presented.Len(), Expected.Len());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const TCHAR Left = Index < Presented.Len() ? Presented[Index] : TEXT('\0');
			const TCHAR Right = Index < Expected.Len() ? Expected[Index] : TEXT('\0');
			Difference |= static_cast<uint32>(Left) ^ static_cast<uint32>(Right);
		}
		return Difference == 0;
	}

	// Lowercase key for the same reason as "origin" above.
	bool IsAuthorized(const FHttpServerRequest& Request, const FString& RequiredToken)
	{
		if (RequiredToken.IsEmpty())
		{
			return true;
		}
		const TArray<FString>* Values = Request.Headers.Find(TEXT("authorization"));
		if (!Values)
		{
			return false;
		}

		// Rejoin before parsing. The engine splits every header value on commas
		// while reading it (HttpConnectionRequestReadContext), so a token with a
		// comma in it arrives in pieces: the first carries the "Bearer " prefix
		// and a truncated secret, the rest carry no prefix and are skipped, and
		// the match could never succeed however correct the token was. Joining
		// with the separator it was split on is the exact inverse - and a single
		// credential is all this header is allowed to carry anyway.
		FString Presented = FString::Join(*Values, TEXT(","));
		Presented.TrimStartAndEndInline();
		if (!Presented.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase))
		{
			return false;
		}
		Presented.RightChopInline(FCString::Strlen(TEXT("Bearer ")));
		Presented.TrimStartAndEndInline();
		return SecretsMatch(Presented, RequiredToken);
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

	// Trimmed at both ends because a token that arrived through a shell export or
	// a launcher script often carries a stray space, and the header it is
	// compared against is trimmed the same way.
	AuthToken = FPlatformMisc::GetEnvironmentVariable(TEXT("UPLINK_AUTH_TOKEN"));
	AuthToken.TrimStartAndEndInline();

	// Safety: refuse to start if the engine's HTTP listener has been configured
	// to bind beyond loopback - this server must never be network-reachable.
	auto IsLoopback = [](const FString& Address)
	{
		return Address.IsEmpty() || Address == TEXT("localhost") || Address == TEXT("127.0.0.1");
	};

	FString BindAddress;
	if (GConfig && GConfig->GetString(TEXT("HTTPServer.Listeners"), TEXT("DefaultBindAddress"), BindAddress, GEngineIni))
	{
		if (!IsLoopback(BindAddress))
		{
			UE_LOG(LogUplink, Error,
				TEXT("Refusing to start: [HTTPServer.Listeners] DefaultBindAddress is '%s' (not loopback). Uplink only serves localhost."),
				*BindAddress);
			return false;
		}
	}

	// DefaultBindAddress is not the whole story: the same section takes a
	// ListenerOverrides array whose per-port entry wins over it
	// (HttpServerConfig.cpp). A project carrying an override for this port would
	// bind wide while the check above saw nothing wrong, so read those too.
	TArray<FString> ListenerOverrides;
	if (GConfig)
	{
		GConfig->GetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), ListenerOverrides, GEngineIni);
	}
	for (FString Override : ListenerOverrides)
	{
		Override.TrimStartAndEndInline();
		Override.ReplaceInline(TEXT("("), TEXT(""));
		Override.ReplaceInline(TEXT(")"), TEXT(""));

		uint32 OverridePort = 0;
		if (!FParse::Value(*Override, TEXT("Port="), OverridePort) || OverridePort != Port)
		{
			continue;
		}
		FString OverrideAddress;
		if (FParse::Value(*Override, TEXT("BindAddress="), OverrideAddress) && !IsLoopback(OverrideAddress))
		{
			UE_LOG(LogUplink, Error,
				TEXT("Refusing to start: [HTTPServer.Listeners] ListenerOverrides binds port %u to '%s' (not loopback). Uplink only serves localhost."),
				Port, *OverrideAddress);
			return false;
		}
		break;
	}

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	Router = HttpServerModule.GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		UE_LOG(LogUplink, Error,
			TEXT("Could not bind port %u - is another editor with Uplink already running? This instance's tools are disabled."),
			Port);
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

	// Which mode this is in gets said every time, so a user who exported a token
	// never has to wonder whether the editor picked it up. The token itself is
	// not logged - a log file is copied into bug reports.
	if (AuthToken.IsEmpty())
	{
		UE_LOG(LogUplink, Log,
			TEXT("Authentication off: any process on this machine can call Uplink. Set UPLINK_AUTH_TOKEN before launching the editor to require a bearer token."));
	}
	else
	{
		UE_LOG(LogUplink, Log,
			TEXT("Authentication on: UPLINK_AUTH_TOKEN is set, so every request must send 'Authorization: Bearer <token>'."));

		// The engine splits every header value on commas while parsing, so a
		// token holding one arrives in pieces that no comparison can put back
		// together. Every request then fails against a token the caller sent
		// correctly, which is the hardest kind of wrong to go looking for.
		if (AuthToken.Contains(TEXT(",")))
		{
			UE_LOG(LogUplink, Warning,
				TEXT("UPLINK_AUTH_TOKEN contains a comma, and no request can ever match it. Relaunch with a token made of letters, digits, '-' or '_' - for example the output of: openssl rand -hex 32"));
		}
	}
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

bool FUplinkServer::CheckRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	// 403 and not 401, so the two refusals stay tellable apart: a bad Origin is
	// not something a credential fixes, and every client that reports a 401 -
	// the bridge included - reports it as "your bearer token is wrong".
	if (!IsOriginAllowed(Request))
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Forbidden, TEXT("forbidden"),
			TEXT("Origin not allowed. Uplink answers only origins on this machine: http://localhost:<port>, http://127.0.0.1:<port> or http://[::1]:<port>. A client that is not a browser can send no Origin header at all.")));
		return false;
	}

	if (!IsAuthorized(Request, AuthToken))
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Error(EHttpServerResponseCodes::Denied,
			TEXT("unauthorized"),
			TEXT("This editor was launched with UPLINK_AUTH_TOKEN set, so every request must carry the header 'Authorization: Bearer <that token>'. This one did not send a matching token."));

		// A 401 without this is not a 401 anyone can act on - it is the part that
		// tells a client which scheme to retry with (RFC 7235).
		Response->Headers.Add(TEXT("WWW-Authenticate"), TArray<FString>{ TEXT("Bearer") });
		OnComplete(MoveTemp(Response));
		return false;
	}

	return true;
}

bool FUplinkServer::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), TEXT("uplink"));
	Json->SetStringField(TEXT("version"), UPLINK_VERSION);
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
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("tools"), Registry.BuildMcpToolList());
	OnComplete(JsonResponse(Json));
	return true;
}

bool FUplinkServer::HandleRunTool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}
	FString ToolName = Request.RelativePath.GetPath();
	ToolName.RemoveFromStart(TEXT("/tool"));
	ToolName.RemoveFromStart(TEXT("/"));

	const TSharedPtr<FJsonObject> Params = ParseBody(Request);
	if (!Params.IsValid())
	{
		RefuseRest(OnComplete, TEXT("request body is not valid JSON (or exceeds 2 MB)"));
		return true;
	}

	const UplinkDispatch::FDispatchOutcome Outcome = UplinkDispatch::Begin(Registry, Tasks, ToolName, Params);
	if (!Outcome.bAccepted)
	{
		RefuseRest(OnComplete, Outcome.RefusalMessage);
		return true;
	}

	Tasks.Await(Outcome.TaskId, Outcome.WaitSeconds,
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
	if (!CheckRequest(Request, OnComplete))
	{
		return true;
	}
	return UplinkMcp::Handle(Request, OnComplete, Registry, Tasks);
}
