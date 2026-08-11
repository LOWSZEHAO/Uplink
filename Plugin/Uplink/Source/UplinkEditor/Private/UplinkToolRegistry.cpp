// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkToolRegistry.h"
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
		UE_LOG(LogUplink, Error, TEXT("Invalid tool schema JSON: %s"), *SchemaJson);
		Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
	}
	return Schema;
}
