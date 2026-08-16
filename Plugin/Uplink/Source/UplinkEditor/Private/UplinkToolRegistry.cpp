// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkToolRegistry.h"
#include "UplinkCompat.h"
#include "UplinkEditorModule.h"
#include "Editor.h"
#include "Serialization/JsonSerializer.h"

UWorld* FUplinkToolContext::ResolveWorld(FString& OutError) const
{
	UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;

	if (WorldSpec == TEXT("pie"))
	{
		if (!PieWorld)
		{
			OutError = TEXT("world='pie' requested but no PIE session is running");
			return nullptr;
		}
		return PieWorld;
	}

	if (WorldSpec == TEXT("editor") || WorldSpec.IsEmpty())
	{
		if (WorldSpec.IsEmpty() && PieWorld)
		{
			return PieWorld; // default: PIE if active
		}
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!EditorWorld)
		{
			OutError = TEXT("no editor world available");
		}
		return EditorWorld;
	}

	OutError = FString::Printf(TEXT("unknown world spec '%s' (use 'editor' or 'pie')"), *WorldSpec);
	return nullptr;
}

bool FUplinkToolContext::IsPieWorld() const
{
	UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
	if (WorldSpec == TEXT("pie"))
	{
		return true;
	}
	return WorldSpec.IsEmpty() && PieWorld != nullptr;
}

namespace
{
	/** Wraps a synchronous lambda as an invocation. */
	class FQuickInvocation final : public IUplinkInvocation
	{
	public:
		explicit FQuickInvocation(TFunction<FUplinkToolResult(const FUplinkToolContext&)> InFn)
			: Fn(MoveTemp(InFn))
		{
		}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			Out = Fn(Ctx);
			return EUplinkToolStep::Done;
		}

	private:
		TFunction<FUplinkToolResult(const FUplinkToolContext&)> Fn;
	};
}

void FUplinkToolRegistry::Register(FUplinkToolInfo Info, TFunction<TSharedRef<IUplinkInvocation>()> Factory)
{
	const FString Name = Info.Name;
	if (Tools.Contains(Name))
	{
		UE_LOG(LogUplink, Warning, TEXT("Tool '%s' registered twice; replacing"), *Name);
	}
	Tools.Add(Name, FUplinkToolDef{ MoveTemp(Info), MoveTemp(Factory) });
}

void FUplinkToolRegistry::RegisterQuick(
	const FString& Name,
	const FString& Description,
	const FString& SchemaJson,
	bool bReadOnly,
	TFunction<FUplinkToolResult(const FUplinkToolContext&)> Fn)
{
	FUplinkToolInfo Info;
	Info.Name = Name;
	Info.Description = Description;
	Info.InputSchema = ParseSchema(SchemaJson);
	Info.bReadOnly = bReadOnly;

	TSharedRef<TFunction<FUplinkToolResult(const FUplinkToolContext&)>> Shared =
		MakeShared<TFunction<FUplinkToolResult(const FUplinkToolContext&)>>(MoveTemp(Fn));

	Register(MoveTemp(Info), [Shared]() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FQuickInvocation>(*Shared);
	});
}

const FUplinkToolDef* FUplinkToolRegistry::Find(const FString& Name) const
{
	return Tools.Find(Name);
}

TArray<TSharedPtr<FJsonValue>> FUplinkToolRegistry::BuildMcpToolList() const
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const auto& Pair : Tools)
	{
		const FUplinkToolInfo& Info = Pair.Value.Info;
		TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), Info.Name);
		Tool->SetStringField(TEXT("description"), Info.Description);
		if (Info.InputSchema.IsValid())
		{
			Tool->SetObjectField(TEXT("inputSchema"), Info.InputSchema);
		}
		TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetBoolField(TEXT("readOnlyHint"), Info.bReadOnly);
		Tool->SetObjectField(TEXT("annotations"), Annotations);
		Out.Add(MakeShared<FJsonValueObject>(Tool));
	}
	return Out;
}

TSharedPtr<FJsonObject> FUplinkToolRegistry::ParseSchema(const FString& SchemaJson)
{
	TSharedPtr<FJsonObject> Schema;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaJson);
	if (!FJsonSerializer::Deserialize(Reader, Schema) || !Schema.IsValid())
	{
		// A tool whose schema will not parse is served with no parameters at
		// all, so every option it has becomes undiscoverable. That went
		// unnoticed for two tools, so make it impossible to miss.
		UE_LOG(LogUplink, Error,
			TEXT("Invalid tool schema JSON - this tool will be served WITHOUT PARAMETERS. Check the braces: %s"),
			*SchemaJson);
		ensureMsgf(false, TEXT("Uplink: a tool's input schema is not valid JSON; see the log for which."));
		Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
	}
	return Schema;
}

bool FUplinkToolRegistry::ValidateParams(
	const FUplinkToolInfo& Info, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	if (!Params.IsValid() || !Info.InputSchema.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Info.InputSchema->TryGetObjectField(FStringView(TEXT("properties")), Properties)
		|| !Properties->IsValid())
	{
		return true; // nothing declared, nothing to check against
	}

	// Read by the transport rather than the tool, so they are always allowed.
	static const TSet<FString> TransportParams = { TEXT("world"), TEXT("wait_ms"), TEXT("timeout_s") };

	TArray<FString> Unknown;
	for (const auto& Pair : Params->Values)
	{
		const FString Key = UplinkCompat::JsonKeyToString(Pair.Key);
		if (TransportParams.Contains(Key) || (*Properties)->HasField(Key))
		{
			continue;
		}
		Unknown.Add(Key);
	}
	if (Unknown.Num() == 0)
	{
		return true;
	}

	// Name the accepted parameters, and point at the nearest one - a rejected
	// call should tell the caller what to type instead of what not to.
	TArray<FString> Accepted;
	for (const auto& Pair : (*Properties)->Values)
	{
		Accepted.Add(UplinkCompat::JsonKeyToString(Pair.Key));
	}
	Accepted.Sort();

	FString Suggestions;
	for (const FString& Bad : Unknown)
	{
		const FString* Closest = nullptr;
		int32 BestDistance = MAX_int32;
		for (const FString& Candidate : Accepted)
		{
			// Cheap nearness: a shared prefix or one containing the other is
			// what a typo or a wrong-but-related name usually looks like.
			const bool bRelated = Candidate.Contains(Bad, ESearchCase::IgnoreCase)
				|| Bad.Contains(Candidate, ESearchCase::IgnoreCase);
			const int32 Distance = bRelated ? FMath::Abs(Candidate.Len() - Bad.Len()) : MAX_int32;
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Closest = &Candidate;
			}
		}
		if (Closest)
		{
			Suggestions += FString::Printf(TEXT(" Did you mean '%s' instead of '%s'?"), **Closest, *Bad);
		}
	}

	OutError = FString::Printf(
		TEXT("unknown parameter%s for %s: %s.%s This tool accepts: %s"),
		Unknown.Num() > 1 ? TEXT("s") : TEXT(""),
		*Info.Name, *FString::Join(Unknown, TEXT(", ")), *Suggestions,
		*FString::Join(Accepted, TEXT(", ")));
	return false;
}
