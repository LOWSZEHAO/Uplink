// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkGraphWires.h"
#include "UplinkCompat.h"
#include "UplinkEditorModule.h"

#include "BlueprintConnectionDrawingPolicy.h"
#include "ConnectionDrawingPolicy.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Layout/Children.h"
#include "NodeFactory.h"
#include "Rendering/DrawElements.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

static TAutoConsoleVariable<int32> CVarUplinkTidyWires(
	TEXT("uplink.TidyWires"), 1,
	TEXT("1 = graph wires draw as straight lines with rounded 90-degree elbows (all graph editors); 0 = engine default splines."));

static TAutoConsoleVariable<int32> CVarUplinkTidyWiresDebug(
	TEXT("uplink.TidyWires.Debug"), 0,
	TEXT("1 = log panel hooks and per-schema policy decisions."));

namespace
{
	/**
	 * Shared orthogonal wire drawing, mixed into a concrete policy class.
	 * Routes: one straight segment when the pins are level, otherwise
	 * horizontal runs joined by rounded vertical elbows. Styling (color,
	 * thickness, bubbles) is whatever the underlying policy decided.
	 */
	template <typename TBase>
	class TUplinkOrthoPolicy : public TBase
	{
	public:
		template <typename... TArgs>
		TUplinkOrthoPolicy(TArgs&&... Args)
			: TBase(Forward<TArgs>(Args)...)
		{
		}

		// The engine's deprecation shims dispatch through the OLD FVector2D
		// virtual on some paths (base DrawSplineWithArrow passes an
		// FDeprecateVector2DParameter) and the new FVector2f virtual on others
		// (the kismet policy). Override both or half the editors keep splines.
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		virtual void DrawConnection(int32 LayerId, const FVector2D& Start, const FVector2D& End, const FConnectionParams& Params) override
		{
			DrawConnection(LayerId, FVector2f(Start), FVector2f(End), Params);
		}
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		virtual void DrawConnection(int32 LayerId, const FVector2f& Start, const FVector2f& End, const FConnectionParams& Params) override
		{
			TArray<FVector2f> Points;
			BuildRoute(Start, End, Points);
			UpdateHover(Points, Params);

			FSlateDrawElement::MakeLines(
				this->DrawElementsList, LayerId, FPaintGeometry(), Points,
				ESlateDrawEffect::None, Params.WireColor, /*bAntialias=*/true, Params.WireThickness);

			if (Params.bDrawBubbles)
			{
				DrawBubbles(LayerId, Points, Params);
			}
		}

	private:
		void BuildRoute(const FVector2f& Start, const FVector2f& End, TArray<FVector2f>& Out) const
		{
			const float Stub = 14.0f * this->ZoomFactor;

			TArray<FVector2f, TInlineAllocator<8>> Corners;
			Corners.Add(Start);
			if (FMath::Abs(Start.Y - End.Y) <= 1.5f && End.X >= Start.X)
			{
				// level pins - one straight segment
			}
			else if (End.X - Start.X >= Stub * 2.0f)
			{
				const float MidX = (Start.X + End.X) * 0.5f;
				Corners.Add(FVector2f(MidX, Start.Y));
				Corners.Add(FVector2f(MidX, End.Y));
			}
			else
			{
				// target is behind the source - route around with stubs
				const float OutX = Start.X + Stub;
				const float InX = End.X - Stub;
				const float MidY = (Start.Y + End.Y) * 0.5f;
				Corners.Add(FVector2f(OutX, Start.Y));
				Corners.Add(FVector2f(OutX, MidY));
				Corners.Add(FVector2f(InX, MidY));
				Corners.Add(FVector2f(InX, End.Y));
			}
			Corners.Add(End);

			// Round each interior corner with a small quadratic arc.
			const float CornerRadius = 10.0f * this->ZoomFactor;
			Out.Reset();
			Out.Add(Corners[0]);
			for (int32 Index = 1; Index < Corners.Num() - 1; ++Index)
			{
				const FVector2f Prev = Corners[Index - 1];
				const FVector2f Corner = Corners[Index];
				const FVector2f Next = Corners[Index + 1];
				const float InLength = (Corner - Prev).Size();
				const float OutLength = (Next - Corner).Size();
				const float Radius = FMath::Min3(CornerRadius, InLength * 0.5f, OutLength * 0.5f);
				if (Radius < 1.0f || InLength < KINDA_SMALL_NUMBER || OutLength < KINDA_SMALL_NUMBER)
				{
					Out.Add(Corner);
					continue;
				}
				const FVector2f ArcStart = Corner - (Corner - Prev) * (Radius / InLength);
				const FVector2f ArcEnd = Corner + (Next - Corner) * (Radius / OutLength);
				Out.Add(ArcStart);
				for (float T = 0.25f; T < 1.0f; T += 0.25f)
				{
					const float U = 1.0f - T;
					Out.Add(ArcStart * (U * U) + Corner * (2.0f * U * T) + ArcEnd * (T * T));
				}
				Out.Add(ArcEnd);
			}
			Out.Add(Corners.Last());
		}

		void UpdateHover(const TArray<FVector2f>& Points, const FConnectionParams& Params)
		{
			if (!Params.AssociatedPin1 && !Params.AssociatedPin2)
			{
				return;
			}
#if UPLINK_UE_AT_LEAST(5, 8)
			const FVector2f Mouse = this->AbsoluteMousePosition;
#else
			const FVector2f Mouse = this->LocalMousePosition;
#endif
			float BestDistSquared = TNumericLimits<float>::Max();
			for (int32 Index = 0; Index < Points.Num() - 1; ++Index)
			{
				const FVector2f Closest = FMath::ClosestPointOnSegment2D(Mouse, Points[Index], Points[Index + 1]);
				BestDistSquared = FMath::Min(BestDistSquared, (Mouse - Closest).SizeSquared());
			}

			const float Tolerance = FMath::Max(Params.WireThickness * 2.0f, 10.0f) * this->ZoomFactor;
			if (BestDistSquared < Tolerance * Tolerance && BestDistSquared < HoverBestDistSquared)
			{
				HoverBestDistSquared = BestDistSquared;
				this->SplineOverlapResult = FGraphSplineOverlapResult(
					Params.AssociatedPin1, Params.AssociatedPin2, BestDistSquared,
					(Mouse - Points[0]).SizeSquared(), (Mouse - Points.Last()).SizeSquared(), true);
			}
		}

		void DrawBubbles(int32 LayerId, const TArray<FVector2f>& Points, const FConnectionParams& Params)
		{
			float Length = 0.0f;
			for (int32 Index = 0; Index < Points.Num() - 1; ++Index)
			{
				Length += (Points[Index + 1] - Points[Index]).Size();
			}
			if (Length <= KINDA_SMALL_NUMBER || !this->BubbleImage)
			{
				return;
			}

			const float BubbleSpacing = 64.0f * this->ZoomFactor;
			const float BubbleSpeed = 192.0f * this->ZoomFactor;
			const FVector2f BubbleSize = FVector2f(this->BubbleImage->ImageSize) * this->ZoomFactor * 0.2f * Params.WireThickness;
			const float Time = static_cast<float>(FPlatformTime::Seconds() - GStartTime);
			const float BubbleOffset = FMath::Fmod(Time * BubbleSpeed, BubbleSpacing);
			const int32 NumBubbles = FMath::CeilToInt(Length / BubbleSpacing);

			for (int32 Index = 0; Index < NumBubbles; ++Index)
			{
				const float Distance = (static_cast<float>(Index) * BubbleSpacing) + BubbleOffset;
				if (Distance >= Length)
				{
					continue;
				}
				FVector2f BubblePos = PointAtDistance(Points, Distance) - (BubbleSize * 0.5f);
				FSlateDrawElement::MakeBox(
					this->DrawElementsList, LayerId, FPaintGeometry(BubblePos, BubbleSize, this->ZoomFactor),
					this->BubbleImage, ESlateDrawEffect::None, Params.WireColor);
			}
		}

		static FVector2f PointAtDistance(const TArray<FVector2f>& Points, float Distance)
		{
			for (int32 Index = 0; Index < Points.Num() - 1; ++Index)
			{
				const float SegmentLength = (Points[Index + 1] - Points[Index]).Size();
				if (Distance <= SegmentLength && SegmentLength > KINDA_SMALL_NUMBER)
				{
					return FMath::Lerp(Points[Index], Points[Index + 1], Distance / SegmentLength);
				}
				Distance -= SegmentLength;
			}
			return Points.Last();
		}

		float HoverBestDistSquared = TNumericLimits<float>::Max();
	};

	/**
	 * Ortho wires for every non-Blueprint graph editor (materials, Niagara
	 * scripts, sound cues, behavior trees, ...). The schema's own policy is
	 * kept alive purely as the styling oracle - its DetermineWiringStyle
	 * supplies each editor's wire colors, thickness and bubble flags - while
	 * this policy draws the geometry.
	 */
	class FUplinkOrthoGenericPolicy : public TUplinkOrthoPolicy<FConnectionDrawingPolicy>
	{
	public:
		FUplinkOrthoGenericPolicy(TUniquePtr<FConnectionDrawingPolicy> InStyleSource,
			int32 InBackLayerID, int32 InFrontLayerID, float InZoomFactor,
			const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements)
			: TUplinkOrthoPolicy<FConnectionDrawingPolicy>(InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements)
			, StyleSource(MoveTemp(InStyleSource))
		{
		}

		virtual void DetermineWiringStyle(UEdGraphPin* OutputPin, UEdGraphPin* InputPin, FConnectionParams& Params) override
		{
			if (StyleSource.IsValid())
			{
				StyleSource->DetermineWiringStyle(OutputPin, InputPin, Params);
			}
			else
			{
				FConnectionDrawingPolicy::DetermineWiringStyle(OutputPin, InputPin, Params);
			}
		}

	private:
		TUniquePtr<FConnectionDrawingPolicy> StyleSource;
	};
}

/** Node factory that swaps only the wire policy; nodes and pins stay stock. */
class FUplinkWireNodeFactory : public FGraphNodeFactory
{
public:
	virtual FConnectionDrawingPolicy* CreateConnectionPolicy(const UEdGraphSchema* Schema, int32 InBackLayerID, int32 InFrontLayerID,
		float InZoomFactor, const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements, UEdGraph* InGraphObj) override
	{
		if (CVarUplinkTidyWiresDebug.GetValueOnGameThread() != 0 && Schema && !LoggedSchemas.Contains(Schema->GetClass()->GetFName()))
		{
			LoggedSchemas.Add(Schema->GetClass()->GetFName());
			UE_LOG(LogUplink, Display, TEXT("TidyWires: policy requested for schema %s (K2: %s)"),
				*Schema->GetClass()->GetName(),
				Schema->IsA(UEdGraphSchema_K2::StaticClass()) ? TEXT("yes") : TEXT("no"));
		}
		if (CVarUplinkTidyWires.GetValueOnGameThread() == 0 || !Schema)
		{
			return FGraphNodeFactory::CreateConnectionPolicy(
				Schema, InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements, InGraphObj);
		}

		// Blueprint-family schemas: subclass the kismet policy directly so the
		// execution roadmap, exec-wire emphasis and debug flow stay intact.
		if (Schema->IsA(UEdGraphSchema_K2::StaticClass()))
		{
			return new TUplinkOrthoPolicy<FKismetConnectionDrawingPolicy>(
				InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements, InGraphObj);
		}

		// Everything else: the schema's policy becomes the style source.
		TUniquePtr<FConnectionDrawingPolicy> StyleSource(FGraphNodeFactory::CreateConnectionPolicy(
			Schema, InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements, InGraphObj));
		return new FUplinkOrthoGenericPolicy(
			MoveTemp(StyleSource), InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements);
	}

private:
	TSet<FName> LoggedSchemas;
};

void FUplinkGraphWires::Startup()
{
	Factory = MakeShared<FUplinkWireNodeFactory>();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUplinkGraphWires::Sweep), 1.0f);
}

void FUplinkGraphWires::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	Factory.Reset();
}

bool FUplinkGraphWires::Sweep(float DeltaTime)
{
	if (!FSlateApplication::IsInitialized() || !Factory.IsValid())
	{
		return true;
	}

	TFunction<void(const TSharedRef<SWidget>&, int32)> Visit;
	Visit = [this, &Visit](const TSharedRef<SWidget>& Widget, int32 Depth)
	{
		if (Depth > 60)
		{
			return;
		}
		if (Widget->GetTypeAsString() == TEXT("SGraphPanel"))
		{
			StaticCastSharedRef<SGraphPanel>(Widget)->SetNodeFactory(Factory.ToSharedRef());
			if (CVarUplinkTidyWiresDebug.GetValueOnGameThread() != 0)
			{
				UE_LOG(LogUplink, Display, TEXT("TidyWires: hooked panel %p"), &Widget.Get());
			}
			return; // panels do not nest
		}
		FChildren* Children = Widget->GetAllChildren();
		if (!Children)
		{
			return;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			Visit(Children->GetChildAt(Index), Depth + 1);
		}
	};

	for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
	{
		Visit(Window, 0);
	}
	return true;
}
