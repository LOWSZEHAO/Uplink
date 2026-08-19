// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkToolRegistry.h"
#include "UplinkCompat.h"
#include "UplinkEditorModule.h"
#include "UplinkToolProvider.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Serialization/JsonSerializer.h"

namespace UplinkWorlds
{
	/** EWorldType as a word, for a caller reading a list of worlds. */
	static const TCHAR* TypeName(EWorldType::Type Type)
	{
		switch (Type)
		{
		case EWorldType::Editor:        return TEXT("Editor");
		case EWorldType::PIE:           return TEXT("PIE");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		case EWorldType::GamePreview:   return TEXT("GamePreview");
		case EWorldType::Game:          return TEXT("Game");
		case EWorldType::GameRPC:       return TEXT("GameRPC");
		case EWorldType::Inactive:      return TEXT("Inactive");
		default:                        return TEXT("None");
		}
	}

	/** Id prefix for a world that is neither the editor world nor a PIE instance. */
	static const TCHAR* KindToken(EWorldType::Type Type)
	{
		switch (Type)
		{
		case EWorldType::EditorPreview:
		case EWorldType::GamePreview: return TEXT("preview");
		case EWorldType::Game:        return TEXT("game");
		case EWorldType::GameRPC:     return TEXT("rpc");
		case EWorldType::Inactive:    return TEXT("inactive");
		default:                      return TEXT("world");
		}
	}

	static const TCHAR* NetModeName(ENetMode Mode)
	{
		switch (Mode)
		{
		case NM_Standalone:      return TEXT("Standalone");
		case NM_DedicatedServer: return TEXT("DedicatedServer");
		case NM_ListenServer:    return TEXT("ListenServer");
		case NM_Client:          return TEXT("Client");
		default:                 return TEXT("Unknown");
		}
	}

	/**
	 * What an omitted world parameter means, in one place. ResolveWorld and the
	 * worlds tool both need it, and if they disagreed the tool would mark a row
	 * as the default that calls do not actually go to.
	 */
	static UWorld* DefaultWorld()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		if (UWorld* PieWorld = GEditor->PlayWorld.Get())
		{
			return PieWorld;
		}
		return GEditor->GetEditorWorldContext().World();
	}

	TArray<FUplinkWorldEntry> Enumerate()
	{
		TArray<FUplinkWorldEntry> Entries;
		if (!GEngine)
		{
			return Entries;
		}

		const UWorld* Default = DefaultWorld();
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World)
			{
				continue; // a context between maps holds no world yet
			}

			FUplinkWorldEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.World = World;
			Entry.Type = TypeName(World->WorldType);
			Entry.NetMode = NetModeName(World->GetNetMode());
			// GetMapName keeps the UEDPIE_0_ prefix the engine puts on a
			// duplicated play world. The instance is already reported on its own
			// field, so leaving it in the name only makes the same level look
			// like a different one depending on which world you asked about.
			Entry.Map = UWorld::RemovePIEPrefix(World->GetMapName());
			Entry.bPlayWorld = World->IsGameWorld();
			Entry.bDefault = (World == Default);

			if (World->WorldType == EWorldType::Editor)
			{
				Entry.Id = TEXT("editor");
			}
			else if (World->WorldType == EWorldType::PIE)
			{
				// The instance index is the engine's own name for "which
				// client", so a caller reading a log or a net-mode column can
				// address the same instance without counting rows.
				Entry.PieInstance = Context.PIEInstance;
				Entry.Id = Context.PIEInstance >= 0
					? FString::Printf(TEXT("pie:%d"), Context.PIEInstance)
					: TEXT("pie");
				Entry.bSimulating = GEditor && GEditor->bIsSimulatingInEditor;
			}
			else
			{
				// A preview world has no map and no instance number, so its id
				// is keyed on the world object's own name: unique among live
				// objects and fixed for as long as the world exists.
				Entry.Id = FString::Printf(TEXT("%s:%s"), KindToken(World->WorldType), *World->GetName());
			}
		}

		// Editor, then PIE instances in order, then the rest - the order a
		// reader scans looking for the world they meant.
		Entries.StableSort([](const FUplinkWorldEntry& A, const FUplinkWorldEntry& B)
		{
			auto Rank = [](const FUplinkWorldEntry& Entry) -> int32
			{
				if (Entry.Type == TEXT("Editor"))
				{
					return 0;
				}
				return Entry.Type == TEXT("PIE") ? 1 : 2;
			};
			const int32 RankA = Rank(A);
			const int32 RankB = Rank(B);
			return RankA != RankB ? RankA < RankB : A.PieInstance < B.PieInstance;
		});
		return Entries;
	}
}

UWorld* FUplinkToolContext::ResolveWorld(FString& OutError) const
{
	// "pie" keeps meaning "whichever world the editor is playing in", not a
	// particular instance. Callers pass it constantly and none of them know or
	// care how many instances are up.
	if (WorldSpec == TEXT("pie"))
	{
		UWorld* PieWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!PieWorld)
		{
			OutError = TEXT("world='pie' requested but no PIE session is running");
			return nullptr;
		}
		return PieWorld;
	}

	if (WorldSpec.IsEmpty())
	{
		UWorld* Default = UplinkWorlds::DefaultWorld();
		if (!Default)
		{
			OutError = TEXT("no editor world available");
		}
		return Default;
	}

	if (WorldSpec == TEXT("editor"))
	{
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!EditorWorld)
		{
			OutError = TEXT("no editor world available");
		}
		return EditorWorld;
	}

	// Anything else is an id from the worlds tool - the only way to name the
	// second PIE instance, or the preview world inside an asset editor.
	const TArray<FUplinkWorldEntry> Worlds = UplinkWorlds::Enumerate();
	for (const FUplinkWorldEntry& Entry : Worlds)
	{
		if (Entry.Id == WorldSpec)
		{
			return Entry.World;
		}
	}

	TArray<FString> Ids;
	for (const FUplinkWorldEntry& Entry : Worlds)
	{
		Ids.Add(Entry.Id);
	}
	OutError = FString::Printf(
		TEXT("unknown world '%s'. Worlds now: %s. 'editor' and 'pie' also work, 'pie' meaning the session in play."),
		*WorldSpec, Ids.Num() ? *FString::Join(Ids, TEXT(", ")) : TEXT("(none)"));
	return nullptr;
}

bool FUplinkToolContext::IsPieWorld() const
{
	if (WorldSpec == TEXT("pie"))
	{
		return true;
	}
	if (WorldSpec.IsEmpty())
	{
		return GEditor && GEditor->PlayWorld != nullptr;
	}
	if (WorldSpec == TEXT("editor"))
	{
		return false;
	}

	// An id can name a play world just as well as "pie" can, and this is what
	// decides whether the task manager opens a transaction around the call.
	// Answering "editor" for pie:1 would wrap every edit to that instance in an
	// undo the engine then throws away.
	for (const FUplinkWorldEntry& Entry : UplinkWorlds::Enumerate())
	{
		if (Entry.Id == WorldSpec)
		{
			return Entry.bPlayWorld;
		}
	}
	return false;
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

	/** "Uplink 0.26.0", "MyPlugin", "an unknown provider" - for a log line. */
	FString UplinkProvenanceToText(const FUplinkToolProvenance& Origin)
	{
		if (Origin.Name.IsEmpty())
		{
			return TEXT("an unknown provider");
		}
		return Origin.Version.IsEmpty()
			? Origin.Name
			: FString::Printf(TEXT("%s %s"), *Origin.Name, *Origin.Version);
	}
}

void FUplinkToolRegistry::Register(FUplinkToolInfo Info, TFunction<TSharedRef<IUplinkInvocation>()> Factory)
{
	const FString Name = Info.Name;

	// Fail closed. A tool whose schema did not parse would be served with an
	// empty parameter list, and an interface that lies is worse than a missing
	// one: the caller cannot see the options, so every call silently does the
	// default thing and reads as a success. Dropping the tool costs that one
	// tool; serving it costs wrong results nobody can attribute.
	if (!Info.InputSchema.IsValid())
	{
		SkippedSchemaTools.Add(Name);
		UE_LOG(LogUplink, Error,
			TEXT("Tool '%s' NOT REGISTERED: its input schema is missing or is not valid JSON. ")
			TEXT("Check the braces in its schema literal - a tool that takes no parameters still ")
			TEXT("needs {\"type\":\"object\",\"properties\":{}}."),
			*Name);
		ensureMsgf(false,
			TEXT("Uplink: tool '%s' has an unusable input schema and was not registered."), *Name);
		return;
	}

	if (Tools.Contains(Name))
	{
		const FUplinkToolProvenance* Previous = ToolProvenance.Find(Name);

		FUplinkToolCollision Collision;
		Collision.ToolName = Name;
		Collision.Replaced = Previous ? *Previous : FUplinkToolProvenance();
		Collision.Winner = ActiveProvider;
		ToolCollisions.Add(Collision);

		// The later registration still wins, because reversing that would
		// change which tool answers a call in every install that already has
		// one. What is new is that the clash is attributable: both sides are
		// named here, and the pair is kept in Collisions() for a report,
		// because otherwise the choice is made by plugin load order and stated
		// nowhere at all.
		UE_LOG(LogUplink, Warning,
			TEXT("Tool '%s' registered twice: %s replaces %s. The later registration wins, so which one ")
			TEXT("answers depends on plugin load order - rename one of them."),
			*Name,
			*UplinkProvenanceToText(Collision.Winner),
			*UplinkProvenanceToText(Collision.Replaced));
	}
	ToolProvenance.Add(Name, ActiveProvider);
	Tools.Add(Name, FUplinkToolDef{ MoveTemp(Info), MoveTemp(Factory) });
}

void FUplinkToolRegistry::RegisterQuick(
	const FString& Name,
	const FString& Description,
	const FString& SchemaJson,
	bool bReadOnly,
	TFunction<FUplinkToolResult(const FUplinkToolContext&)> Fn,
	bool bTransactional)
{
	FUplinkToolInfo Info;
	Info.Name = Name;
	Info.Description = Description;
	Info.InputSchema = ParseSchema(SchemaJson);
	Info.bReadOnly = bReadOnly;
	Info.bTransactional = bTransactional;
	// A quick tool runs to completion inside one call. There is no moment at
	// which a cancel could arrive and change the outcome, so saying it is
	// cancellable would be a promise nothing can keep.
	Info.bCancellable = false;

	TSharedRef<TFunction<FUplinkToolResult(const FUplinkToolContext&)>> Shared =
		MakeShared<TFunction<FUplinkToolResult(const FUplinkToolContext&)>>(MoveTemp(Fn));

	Register(MoveTemp(Info), [Shared]() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FQuickInvocation>(*Shared);
	});
}

void FUplinkToolRegistry::RegisterFrom(IUplinkToolProvider& Provider)
{
	FUplinkToolProvenance Identity;
	Identity.Name = Provider.GetUplinkProviderName();
	Identity.Version = Provider.GetUplinkProviderVersion();
	if (Identity.Name.IsEmpty())
	{
		// A provider that will not name itself is still not Uplink, and saying
		// that much beats an empty string a reader takes for a built-in.
		Identity.Name = TEXT("unknown provider");
	}

	// Restored rather than cleared: a provider that reaches the registry from
	// inside another provider's call must not silently reattribute the rest of
	// that call to itself.
	const FUplinkToolProvenance Outer = ActiveProvider;
	ActiveProvider = Identity;
	Provider.RegisterUplinkTools(*this);
	ActiveProvider = Outer;
}

const FUplinkToolDef* FUplinkToolRegistry::Find(const FString& Name) const
{
	return Tools.Find(Name);
}

const FUplinkToolProvenance* FUplinkToolRegistry::FindProvenance(const FString& Name) const
{
	return ToolProvenance.Find(Name);
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
		// readOnlyHint and destructiveHint are the ones MCP itself defines, and a
		// client reads them as a pair - "safe to auto-approve" is the absence of
		// both - so both are always stated. The rest are Uplink's own names and
		// appear only when true, to keep a hundred entries readable.
		TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetBoolField(TEXT("readOnlyHint"), Info.bReadOnly);
		Annotations->SetBoolField(TEXT("destructiveHint"), Info.bDestructive);
		if (Info.bArbitraryExecution)
		{
			Annotations->SetBoolField(TEXT("arbitraryExecutionHint"), true);
		}
		if (Info.bRequiresPie)
		{
			Annotations->SetBoolField(TEXT("requiresPieHint"), true);
		}
		if (Info.bLongRunning)
		{
			Annotations->SetBoolField(TEXT("longRunningHint"), true);
		}
		if (Info.bCancellable)
		{
			Annotations->SetBoolField(TEXT("cancellableHint"), true);
		}
		// Built-ins say nothing: a hundred entries all claiming Uplink is noise
		// in a list a client re-reads on every connect. A tool from anywhere
		// else names its plugin, because there is nowhere else to find that out
		// once the tool is sitting in the list next to the built-in ones.
		const FUplinkToolProvenance* Origin = ToolProvenance.Find(Info.Name);
		if (Origin && !Origin->bBuiltIn)
		{
			Annotations->SetStringField(TEXT("provider"), Origin->Name);
		}
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
		// The literal is only visible here, so this is where it gets printed;
		// Register() names the tool and refuses it. Two lines about one fault,
		// but between them they say exactly which text to fix.
		UE_LOG(LogUplink, Error, TEXT("Tool input schema is not valid JSON: %s"), *SchemaJson);
		return nullptr;
	}
	return Schema;
}

namespace
{
	// A pragmatic subset of JSON Schema - the keywords the tool schemas in this
	// plugin actually use. Everything else is skipped on purpose: a check this
	// code cannot make must not turn into a refusal, or a schema written next
	// year starts rejecting calls that were always fine.

	/** JSON has no integer type, so a client meaning 3 routinely sends 3.0. */
	bool IsWholeNumber(double Number)
	{
		return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number), 1.e-9);
	}

	/** A number the way the caller wrote it: 3 stays 3, 3.5 stays 3.5. */
	FString NumberToText(double Number)
	{
		return (IsWholeNumber(Number) && FMath::Abs(Number) < 1.e15)
			? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)))
			: FString::SanitizeFloat(Number);
	}

	/** The bare value, for listing an enum or quoting a bound. */
	FString RenderJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		switch (Value->Type)
		{
		case EJson::String:  return Value->AsString();
		case EJson::Number:  return NumberToText(Value->AsNumber());
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Array:   return TEXT("an array");
		case EJson::Object:  return TEXT("an object");
		default:             return TEXT("null");
		}
	}

	/** The value with its type in front - the "got" half of a type complaint. */
	FString DescribeJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("nothing");
		}
		switch (Value->Type)
		{
		case EJson::String:  return FString::Printf(TEXT("string \"%s\""), *Value->AsString());
		case EJson::Number:  return FString::Printf(TEXT("number %s"), *NumberToText(Value->AsNumber()));
		case EJson::Boolean: return Value->AsBool() ? TEXT("boolean true") : TEXT("boolean false");
		case EJson::Array:   return TEXT("an array");
		case EJson::Object:  return TEXT("an object");
		default:             return TEXT("null");
		}
	}

	/** Unknown type names answer true: an unmodelled keyword is not a failure. */
	bool MatchesDeclaredType(const FString& DeclaredType, const TSharedPtr<FJsonValue>& Value)
	{
		if (DeclaredType == TEXT("string"))
		{
			return Value->Type == EJson::String;
		}
		if (DeclaredType == TEXT("number"))
		{
			return Value->Type == EJson::Number;
		}
		if (DeclaredType == TEXT("integer"))
		{
			return Value->Type == EJson::Number && IsWholeNumber(Value->AsNumber());
		}
		if (DeclaredType == TEXT("boolean"))
		{
			return Value->Type == EJson::Boolean;
		}
		if (DeclaredType == TEXT("array"))
		{
			return Value->Type == EJson::Array;
		}
		if (DeclaredType == TEXT("object"))
		{
			return Value->Type == EJson::Object;
		}
		return true;
	}

	/**
	 * Scalars only; an enum of objects is not something this subset judges.
	 *
	 * Strings compare without regard to case because that is how the tools
	 * themselves read these values - every op/kind/mode/world branch in the
	 * plugin is an FString ==, which ignores case. Refusing "Add_Variable"
	 * here would reject a call the tool would have carried out.
	 */
	bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		if (!A.IsValid() || !B.IsValid() || A->Type != B->Type)
		{
			return false;
		}
		switch (A->Type)
		{
		case EJson::String:  return A->AsString().Equals(B->AsString(), ESearchCase::IgnoreCase);
		case EJson::Number:  return FMath::IsNearlyEqual(A->AsNumber(), B->AsNumber(), 1.e-9);
		case EJson::Boolean: return A->AsBool() == B->AsBool();
		default:             return false;
		}
	}

	/** "steps[0]" + "tool" reads back to the caller as "steps[0].tool". */
	FString QualifyPath(const FString& Path, const FString& Name)
	{
		return Path.IsEmpty() ? Name : Path + TEXT(".") + Name;
	}

	bool ValidateObjectAgainstSchema(
		const FString& Path,
		const TSharedPtr<FJsonObject>& Object,
		const TSharedPtr<FJsonObject>& Schema,
		FString& OutError);

	bool ValidateAgainstSchema(
		const FString& Path,
		const TSharedPtr<FJsonValue>& Value,
		const TSharedPtr<FJsonObject>& Schema,
		FString& OutError)
	{
		if (!Value.IsValid() || !Schema.IsValid())
		{
			return true;
		}

		// "type" as an array of names, or absent, lands here as a skipped check.
		FString DeclaredType;
		if (Schema->TryGetStringField(FStringView(TEXT("type")), DeclaredType)
			&& !MatchesDeclaredType(DeclaredType, Value))
		{
			// An explicit null is nearly always a client filling in an optional
			// field it has no value for, so say what to do about it.
			OutError = (Value->Type == EJson::Null)
				? FString::Printf(
					TEXT("parameter '%s' expects %s, got null - leave the parameter out instead of sending null"),
					*Path, *DeclaredType)
				: FString::Printf(TEXT("parameter '%s' expects %s, got %s"),
					*Path, *DeclaredType, *DescribeJsonValue(Value));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Allowed = nullptr;
		if (Schema->TryGetArrayField(FStringView(TEXT("enum")), Allowed) && Allowed->Num() > 0)
		{
			bool bComparable = true;
			bool bMatched = false;
			TArray<FString> Options;
			for (const TSharedPtr<FJsonValue>& Option : *Allowed)
			{
				if (!Option.IsValid() || Option->Type == EJson::Array || Option->Type == EJson::Object)
				{
					bComparable = false;
					break;
				}
				Options.Add(RenderJsonValue(Option));
				bMatched |= JsonValuesEqual(Option, Value);
			}
			if (bComparable && !bMatched)
			{
				OutError = FString::Printf(TEXT("parameter '%s' must be one of: %s - got '%s'"),
					*Path, *FString::Join(Options, TEXT(", ")), *RenderJsonValue(Value));
				return false;
			}
		}

		if (Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			double Bound = 0.0;
			if (Schema->TryGetNumberField(FStringView(TEXT("minimum")), Bound) && Number < Bound)
			{
				OutError = FString::Printf(TEXT("parameter '%s' must be at least %s, got %s"),
					*Path, *NumberToText(Bound), *NumberToText(Number));
				return false;
			}
			if (Schema->TryGetNumberField(FStringView(TEXT("maximum")), Bound) && Number > Bound)
			{
				OutError = FString::Printf(TEXT("parameter '%s' must be at most %s, got %s"),
					*Path, *NumberToText(Bound), *NumberToText(Number));
				return false;
			}
		}

		if (Value->Type == EJson::String)
		{
			const int32 Length = Value->AsString().Len();
			double Limit = 0.0;
			if (Schema->TryGetNumberField(FStringView(TEXT("minLength")), Limit) && Length < Limit)
			{
				OutError = FString::Printf(TEXT("parameter '%s' must be at least %s characters, got %d"),
					*Path, *NumberToText(Limit), Length);
				return false;
			}
			if (Schema->TryGetNumberField(FStringView(TEXT("maxLength")), Limit) && Length > Limit)
			{
				OutError = FString::Printf(TEXT("parameter '%s' must be at most %s characters, got %d"),
					*Path, *NumberToText(Limit), Length);
				return false;
			}
		}

		if (Value->Type == EJson::Array)
		{
			// Tuple form ("items" as an array of schemas) is not modelled, and
			// TryGetObjectField leaves it alone.
			const TSharedPtr<FJsonObject>* ItemSchema = nullptr;
			if (Schema->TryGetObjectField(FStringView(TEXT("items")), ItemSchema) && ItemSchema->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
					if (!ValidateAgainstSchema(ItemPath, Items[Index], *ItemSchema, OutError))
					{
						return false;
					}
				}
			}
		}

		if (Value->Type == EJson::Object)
		{
			return ValidateObjectAgainstSchema(Path, Value->AsObject(), Schema, OutError);
		}

		return true;
	}

	bool ValidateObjectAgainstSchema(
		const FString& Path,
		const TSharedPtr<FJsonObject>& Object,
		const TSharedPtr<FJsonObject>& Schema,
		FString& OutError)
	{
		if (!Object.IsValid() || !Schema.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (Schema->TryGetArrayField(FStringView(TEXT("required")), Required))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Required)
			{
				FString Name;
				if (!Entry.IsValid() || !Entry->TryGetString(Name))
				{
					continue;
				}
				if (!Object->HasField(Name))
				{
					OutError = FString::Printf(TEXT("parameter '%s' is required but was not given"),
						*QualifyPath(Path, Name));
					return false;
				}
			}
		}

		// Only declared properties are visited, so wait_ms and timeout_s - read
		// by the transport and never by a tool - go past unexamined. A tool
		// that does declare 'world' has it checked like any other parameter,
		// which is what the caller wants: the enum on it is real.
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Schema->TryGetObjectField(FStringView(TEXT("properties")), Properties)
			|| !Properties->IsValid())
		{
			return true;
		}

		for (const auto& Pair : (*Properties)->Values)
		{
			const FString Name = UplinkCompat::JsonKeyToString(Pair.Key);
			const TSharedPtr<FJsonValue> Supplied = Object->TryGetField(FStringView(Name));
			if (!Supplied.IsValid() || !Pair.Value.IsValid())
			{
				continue;
			}

			// TryGetObject rather than AsObject: a property declared as
			// anything but an object is a fragment to walk past, and AsObject
			// would log an engine error on the way.
			const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
			if (!Pair.Value->TryGetObject(PropertySchema) || !PropertySchema->IsValid())
			{
				continue;
			}
			if (!ValidateAgainstSchema(QualifyPath(Path, Name), Supplied, *PropertySchema, OutError))
			{
				return false;
			}
		}
		return true;
	}
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
		// Names first, then types. A caller who got the name wrong needs to
		// hear that and nothing else, and the type of a parameter the tool
		// never had is not worth a sentence.
		return ValidateObjectAgainstSchema(FString(), Params, Info.InputSchema, OutError);
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
