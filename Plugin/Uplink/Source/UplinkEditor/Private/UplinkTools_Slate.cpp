// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.
// Editor UI tools: ui_tree (query the live Slate hierarchy) and
// capture_widget (screenshot any editor window or panel, not just viewports).
// Together they let a client see every part of the editor UI - asset editor
// previews, graph panels, detail panels - without desktop-level screenshots.

#include "UplinkTools.h"
#include "UplinkSlateScreenshot.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

using namespace UplinkToolUtil;

namespace
{
	/** Text content for the widget types that carry human-readable labels. */
	FString WidgetText(const TSharedRef<SWidget>& Widget)
	{
		const FString Type = Widget->GetTypeAsString();
		FString Text;
		if (Type == TEXT("STextBlock"))
		{
			Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		}
		else if (Type == TEXT("SDockTab"))
		{
			Text = StaticCastSharedRef<SDockTab>(Widget)->GetTabLabel().ToString();
		}
		else if (Type == TEXT("SWindow"))
		{
			Text = StaticCastSharedRef<SWindow>(Widget)->GetTitle().ToString();
		}
		if (Text.Len() > 120)
		{
			Text = Text.Left(117) + TEXT("...");
		}
		return Text;
	}

	/**
	 * Every reachable window. Asset editors can spawn as CHILD windows of the
	 * main frame (5.7 does this; 5.8 docks them as tabs instead) and child
	 * windows are absent from the top-level list - walk them in too.
	 */
	TArray<TSharedRef<SWindow>> TopLevelWindows()
	{
		TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
		for (int32 Index = 0; Index < Windows.Num(); ++Index)
		{
			for (const TSharedRef<SWindow>& Child : Windows[Index]->GetChildWindows())
			{
				Windows.Add(Child);
			}
		}
		return Windows;
	}

	/**
	 * The leading 'wN' of a path handed out by ui_tree, or INDEX_NONE when the
	 * path carries none. The index is only meaningful against the window list
	 * as it stood when the path was produced.
	 */
	int32 WindowIndexFromPath(const FString& Path)
	{
		TArray<FString> Steps;
		Path.ParseIntoArray(Steps, TEXT("/"));
		if (Steps.Num() == 0 || !Steps[0].StartsWith(TEXT("w")))
		{
			return INDEX_NONE;
		}
		const FString Digits = Steps[0].RightChop(1);
		return Digits.IsNumeric() ? FCString::Atoi(*Digits) : INDEX_NONE;
	}

	/**
	 * Resolve which window a tool call targets, in order:
	 *  - 'window' title substring when given (first match)
	 *  - the wN a ui_tree path carries, when it has one
	 *  - otherwise the largest window on screen (in practice the main editor frame).
	 */
	TSharedPtr<SWindow> ResolveWindow(const FString& TitleFilter, int32& OutIndex, FString& OutError,
		int32 PathWindowIndex = INDEX_NONE)
	{
		const TArray<TSharedRef<SWindow>> Windows = TopLevelWindows();
		if (Windows.Num() == 0)
		{
			OutError = TEXT("no top-level Slate windows exist");
			return nullptr;
		}

		// An explicit 'window' wins; otherwise the wN a ui_tree path carries is
		// the window that path was measured in. Ignoring it meant a path taken
		// from an asset editor was walked from whichever window was largest -
		// usually the main frame - and the indices below it addressed a
		// different widget entirely, which capture_widget then screenshotted
		// and reported as the one asked for.
		if (TitleFilter.IsEmpty() && PathWindowIndex != INDEX_NONE)
		{
			if (!Windows.IsValidIndex(PathWindowIndex))
			{
				OutError = FString::Printf(
					TEXT("this path names window w%d and there are %d open now. Window indices are only valid ")
					TEXT("while the same windows are open - take a fresh path from ui_tree."),
					PathWindowIndex, Windows.Num());
				return nullptr;
			}
			OutIndex = PathWindowIndex;
			return Windows[PathWindowIndex];
		}

		if (!TitleFilter.IsEmpty())
		{
			for (int32 Index = 0; Index < Windows.Num(); ++Index)
			{
				if (Windows[Index]->GetTitle().ToString().Contains(TitleFilter, ESearchCase::IgnoreCase))
				{
					OutIndex = Index;
					return Windows[Index];
				}
			}
			TArray<FString> Titles;
			for (const TSharedRef<SWindow>& Window : Windows)
			{
				Titles.Add(FString::Printf(TEXT("\"%s\""), *Window->GetTitle().ToString()));
			}
			OutError = FString::Printf(TEXT("no window title contains '%s'. Windows: %s"),
				*TitleFilter, *FString::Join(Titles, TEXT(", ")));
			return nullptr;
		}

		int32 Best = 0;
		float BestArea = -1.0f;
		for (int32 Index = 0; Index < Windows.Num(); ++Index)
		{
			const FVector2D Size = Windows[Index]->GetSizeInScreen();
			const float Area = Size.X * Size.Y;
			if (Area > BestArea)
			{
				BestArea = Area;
				Best = Index;
			}
		}
		OutIndex = Best;
		return Windows[Best];
	}

	/** Follow a '/'-separated child-index path down from a root widget. */
	TSharedPtr<SWidget> WidgetAtPath(const TSharedRef<SWidget>& Root, const FString& Path, FString& OutError)
	{
		TSharedRef<SWidget> Current = Root;
		TArray<FString> Steps;
		Path.ParseIntoArray(Steps, TEXT("/"));
		for (const FString& Step : Steps)
		{
			if (Step.StartsWith(TEXT("w")))
			{
				continue; // window prefix - resolved by ResolveWindow before this walk
			}
			FChildren* Children = Current->GetAllChildren();
			const int32 Index = FCString::Atoi(*Step);
			if (!Children || Index < 0 || Index >= Children->Num())
			{
				OutError = FString::Printf(TEXT("path step '%s' out of range under %s (%d children)"),
					*Step, *Current->GetTypeAsString(), Children ? Children->Num() : 0);
				return nullptr;
			}
			Current = Children->GetChildAt(Index);
		}
		return Current;
	}

	/**
	 * Depth-first search for the first descendant whose type name contains the
	 * filter. Only visible widgets with rendered geometry qualify - editors keep
	 * hidden duplicates around (collapsed viewport panes, inactive tabs), and
	 * those cannot be screenshotted.
	 */
	TSharedPtr<SWidget> FindByType(const TSharedRef<SWidget>& Root, const FString& TypeFilter,
		FString& OutPath, const FString& PathSoFar, int32& Budget)
	{
		if (--Budget <= 0)
		{
			return nullptr;
		}
		if (Root->GetTypeAsString().Contains(TypeFilter, ESearchCase::IgnoreCase)
			&& Root->GetVisibility().IsVisible()
			&& Root->GetCachedGeometry().GetAbsoluteSize().X > 0.0f)
		{
			OutPath = PathSoFar;
			return Root;
		}
		FChildren* Children = Root->GetAllChildren();
		if (!Children)
		{
			return nullptr;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			const FString ChildPath = PathSoFar.IsEmpty()
				? FString::FromInt(Index)
				: PathSoFar + TEXT("/") + FString::FromInt(Index);
			TSharedPtr<SWidget> Found = FindByType(Children->GetChildAt(Index), TypeFilter, OutPath, ChildPath, Budget);
			if (Found.IsValid())
			{
				return Found;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> WidgetToJson(const TSharedRef<SWidget>& Widget, const FString& Path)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("path"), Path);
		Row->SetStringField(TEXT("type"), Widget->GetTypeAsString());

		const FString Text = WidgetText(Widget);
		if (!Text.IsEmpty())
		{
			Row->SetStringField(TEXT("text"), Text);
		}
		if (!Widget->GetVisibility().IsVisible())
		{
			Row->SetBoolField(TEXT("hidden"), true);
		}

		const FGeometry& Geometry = Widget->GetCachedGeometry();
		const FVector2D Pos = Geometry.GetAbsolutePosition();
		const FVector2D Size = Geometry.GetAbsoluteSize();
		if (Size.X > 0.0f && Size.Y > 0.0f)
		{
			TArray<TSharedPtr<FJsonValue>> Rect;
			Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Pos.X)));
			Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Pos.Y)));
			Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Size.X)));
			Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Size.Y)));
			Row->SetArrayField(TEXT("rect"), Rect);
		}
		return Row;
	}

	struct FWalkState
	{
		FString Find;
		int32 MaxDepth = 10;
		int32 MaxNodes = 500;
		int32 Visited = 0;
		bool bTruncated = false;
		TArray<TSharedPtr<FJsonValue>> Out;
	};

	void WalkTree(const TSharedRef<SWidget>& Widget, const FString& Path, int32 Depth, FWalkState& State)
	{
		if (++State.Visited > 250000)
		{
			State.bTruncated = true;
			return;
		}

		bool bEmit = State.Find.IsEmpty();
		if (!bEmit)
		{
			bEmit = Widget->GetTypeAsString().Contains(State.Find, ESearchCase::IgnoreCase)
				|| WidgetText(Widget).Contains(State.Find, ESearchCase::IgnoreCase);
		}
		if (bEmit)
		{
			if (State.Out.Num() >= State.MaxNodes)
			{
				State.bTruncated = true;
				return;
			}
			State.Out.Add(MakeShared<FJsonValueObject>(WidgetToJson(Widget, Path)));
		}

		// Depth limits structure dumps; a 'find' search always goes all the way down.
		if (State.Find.IsEmpty() && Depth >= State.MaxDepth)
		{
			return;
		}
		FChildren* Children = Widget->GetAllChildren();
		if (!Children)
		{
			return;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (State.bTruncated && State.Find.IsEmpty())
			{
				return;
			}
			WalkTree(Children->GetChildAt(Index),
				Path + TEXT("/") + FString::FromInt(Index), Depth + 1, State);
		}
	}
}

void UplinkTools::RegisterSlate(FUplinkToolRegistry& Registry)
{
	Registry.RegisterQuick(
		TEXT("ui_tree"),
		TEXT("Query the editor's live Slate widget hierarchy. Default: structure of the main (largest) window to 'max_depth'. 'window' targets a window by title substring; 'find' searches the whole depth (all windows when no 'window' is given) for widgets whose type or text contains the string - e.g. find:'NiagaraSystemViewport' or find:'Content Browser'. Paths returned here ('w0/1/0/3') are the handles capture_widget uses. 'rect' is [x,y,w,h] in desktop pixels."),
		TEXT(R"json({"type":"object","properties":{"window":{"type":"string","description":"Window title substring (default: largest window; with 'find': all windows)"},"find":{"type":"string","description":"Case-insensitive substring of widget type or text"},"path":{"type":"string","description":"Start the walk at this widget path instead of the window root"},"max_depth":{"type":"number","default":10},"max_nodes":{"type":"number","default":500}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			if (!FSlateApplication::IsInitialized())
			{
				return FUplinkToolResult::Error(TEXT("Slate is not initialized"));
			}

			FWalkState State;
			State.Find = GetString(Ctx.Params, TEXT("find"));
			State.MaxDepth = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_depth"), 10)), 1, 64);
			State.MaxNodes = FMath::Clamp(static_cast<int32>(GetNumber(Ctx.Params, TEXT("max_nodes"), 500)), 1, 5000);

			const FString WindowFilter = GetString(Ctx.Params, TEXT("window"));
			const FString StartPath = GetString(Ctx.Params, TEXT("path"));
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			// A find with neither a window nor a path is a global find.
			if (!State.Find.IsEmpty() && WindowFilter.IsEmpty() && StartPath.IsEmpty())
			{
				const TArray<TSharedRef<SWindow>> Windows = TopLevelWindows();
				for (int32 Index = 0; Index < Windows.Num(); ++Index)
				{
					WalkTree(Windows[Index], FString::Printf(TEXT("w%d"), Index), 0, State);
				}
			}
			else
			{
				FString Error;
				int32 WindowIndex = 0;
				TSharedPtr<SWindow> Window = ResolveWindow(WindowFilter, WindowIndex, Error, WindowIndexFromPath(StartPath));
				if (!Window.IsValid())
				{
					return FUplinkToolResult::Error(Error);
				}
				Data->SetStringField(TEXT("window"), Window->GetTitle().ToString());

				TSharedRef<SWidget> Root = Window.ToSharedRef();
				FString RootPath = FString::Printf(TEXT("w%d"), WindowIndex);
				if (!StartPath.IsEmpty())
				{
					TSharedPtr<SWidget> Start = WidgetAtPath(Root, StartPath, Error);
					if (!Start.IsValid())
					{
						return FUplinkToolResult::Error(Error);
					}
					Root = Start.ToSharedRef();
					RootPath = StartPath.StartsWith(TEXT("w"))
						? StartPath
						: FString::Printf(TEXT("w%d/%s"), WindowIndex, *StartPath);
				}
				WalkTree(Root, RootPath, 0, State);
			}

			Data->SetArrayField(TEXT("widgets"), State.Out);
			Data->SetNumberField(TEXT("count"), State.Out.Num());
			Data->SetBoolField(TEXT("truncated"), State.bTruncated);
			return FUplinkToolResult::Ok(Data);
		});

	Registry.RegisterQuick(
		TEXT("capture_widget"),
		TEXT("Screenshot any editor window or widget as a PNG - asset editor previews, graph panels, detail panels - even when the window is behind others. Target: 'window' (title substring; default largest window) plus either 'path' (from ui_tree) or 'type' (first descendant whose type name contains this, e.g. 'SNiagaraSystemViewport', 'SGraphEditor'). With neither, captures the whole window."),
		TEXT(R"json({"type":"object","properties":{"window":{"type":"string"},"path":{"type":"string","description":"Widget path from ui_tree, e.g. 'w0/1/0/3' or '1/0/3'"},"type":{"type":"string","description":"Type-name substring to find, e.g. 'SGraphEditor'"}}})json"),
		/*bReadOnly=*/true,
		[](const FUplinkToolContext& Ctx) -> FUplinkToolResult
		{
			if (!FSlateApplication::IsInitialized())
			{
				return FUplinkToolResult::Error(TEXT("Slate is not initialized"));
			}

			// Read before resolving the window: the path names the window it was
			// measured in, and that is what decides which one this walks.
			const FString Path = GetString(Ctx.Params, TEXT("path"));

			FString Error;
			int32 WindowIndex = 0;
			TSharedPtr<SWindow> Window = ResolveWindow(GetString(Ctx.Params, TEXT("window")), WindowIndex, Error,
				WindowIndexFromPath(Path));
			if (!Window.IsValid())
			{
				return FUplinkToolResult::Error(Error);
			}

			TSharedRef<SWidget> Target = Window.ToSharedRef();
			FString TargetPath = FString::Printf(TEXT("w%d"), WindowIndex);

			const FString TypeFilter = GetString(Ctx.Params, TEXT("type"));
			if (!Path.IsEmpty())
			{
				TSharedPtr<SWidget> AtPath = WidgetAtPath(Window.ToSharedRef(), Path, Error);
				if (!AtPath.IsValid())
				{
					return FUplinkToolResult::Error(Error);
				}
				Target = AtPath.ToSharedRef();
				TargetPath = Path;
			}
			else if (!TypeFilter.IsEmpty())
			{
				int32 Budget = 250000;
				FString FoundPath;
				TSharedPtr<SWidget> Found = FindByType(Window.ToSharedRef(), TypeFilter, FoundPath, FString(), Budget);
				if (!Found.IsValid())
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("no widget of type containing '%s' in window \"%s\" (use ui_tree with 'find' to look across windows)"),
						*TypeFilter, *Window->GetTitle().ToString()));
				}
				Target = Found.ToSharedRef();
				TargetPath = FString::Printf(TEXT("w%d/%s"), WindowIndex, *FoundPath);
			}

			// TakeScreenshot resolves the widget's path with a check() - feeding it
			// a widget that is not currently rendered would assert and kill the
			// editor (found the hard way with a collapsed level-viewport pane).
			// Validate with the unchecked lookup first and fail politely.
			if (Target != Window.ToSharedRef())
			{
				FWidgetPath PathToWidget;
				if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(Target, PathToWidget))
				{
					return FUplinkToolResult::Error(FString::Printf(
						TEXT("%s at %s is not currently rendered (hidden tab or collapsed pane) and cannot be captured. ")
						TEXT("Pick a visible widget - ui_tree rows without 'hidden' and with a 'rect' are capturable."),
						*Target->GetTypeAsString(), *TargetPath));
				}
			}

			TArray<FColor> Pixels;
			FIntPoint Size = FIntPoint::ZeroValue;
			if (!UplinkSlateScreenshot::Take(Target, Pixels, Size))
			{
				return FUplinkToolResult::Error(FString::Printf(
					TEXT("screenshot failed for %s (widget may have zero size or be unrendered)"),
					*Target->GetTypeAsString()));
			}

			IImageWrapperModule& ImageWrapperModule =
				FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			const TSharedPtr<IImageWrapper> Png = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
			if (!Png.IsValid() || !Png->SetRaw(
				Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8))
			{
				return FUplinkToolResult::Error(TEXT("PNG encoding failed"));
			}

			FUplinkToolResult Out = FUplinkToolResult::Ok();
			Out.Png = Png->GetCompressed(90);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("window"), Window->GetTitle().ToString());
			Data->SetStringField(TEXT("widget"), Target->GetTypeAsString());
			Data->SetStringField(TEXT("path"), TargetPath);
			Data->SetNumberField(TEXT("width"), Size.X);
			Data->SetNumberField(TEXT("height"), Size.Y);
			Out.Data = Data;
			return Out;
		});
}
