// Copyright (c) 2026 Low Sze Hao. MIT License.

#include "UplinkGraphWires.h"
#include "UplinkCompat.h"

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
	TEXT("1 = Blueprint wires draw as straight lines with 90-degree elbows; 0 = engine default splines."));

namespace
{
	/**
	 * Kismet wire policy that replaces the spline with an orthogonal polyline:
	 * a single straight segment when the pins are level, otherwise horizontal
	 * runs joined by vertical elbows. Everything else (exec-flow colors and
	 * thickness, debug bubbles, preview connectors) comes from the base class.
	 */
	class FUplinkOrthoK2Policy : public FKismetConnectionDrawingPolicy
	{
	public:
		FUplinkOrthoK2Policy(int32 InBackLayerID, int32 InFrontLayerID, float InZoomFactor,
			const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements, UEdGraph* InGraphObj)
			: FKismetConnectionDrawingPolicy(InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements, InGraphObj)
		{
		}

		virtual void DrawConnection(int32 LayerId, const FVector2f& Start, const FVector2f& End, const FConnectionParams& Params) override
		{
			TArray<FVector2f> Points;
			BuildRoute(Start, End, Points);

			// Wire hover detection normally happens inside the engine's spline
			// draw; replicate it for the polyline so hover highlight and
			// double-click-to-reroute keep working.
			UpdateHover(Points, Params);

			FSlateDrawElement::MakeLines(
				DrawElementsList, LayerId, FPaintGeometry(), Points,
				ESlateDrawEffect::None, Params.WireColor, /*bAntialias=*/true, Params.WireThickness);

			if (Params.bDrawBubbles)
			{
				DrawBubbles(LayerId, Points, Params);
			}
		}

	private:
		void BuildRoute(const FVector2f& Start, const FVector2f& End, TArray<FVector2f>& Out) const
		{
			const float Stub = 14.0f * ZoomFactor;
			Out.Reset();
			Out.Add(Start);
			if (FMath::Abs(Start.Y - End.Y) <= 1.5f && End.X >= Start.X)
			{
				// level pins - one straight segment
			}
			else if (End.X - Start.X >= Stub * 2.0f)
			{
				const float MidX = (Start.X + End.X) * 0.5f;
				Out.Add(FVector2f(MidX, Start.Y));
				Out.Add(FVector2f(MidX, End.Y));
			}
			else
			{
				// target is behind the source - route around with stubs
				const float OutX = Start.X + Stub;
				const float InX = End.X - Stub;
				const float MidY = (Start.Y + End.Y) * 0.5f;
				Out.Add(FVector2f(OutX, Start.Y));
				Out.Add(FVector2f(OutX, MidY));
				Out.Add(FVector2f(InX, MidY));
				Out.Add(FVector2f(InX, End.Y));
			}
			Out.Add(End);
		}

		void UpdateHover(const TArray<FVector2f>& Points, const FConnectionParams& Params)
		{
			if (!Params.AssociatedPin1 && !Params.AssociatedPin2)
			{
				return;
			}
#if UPLINK_UE_AT_LEAST(5, 8)
			const FVector2f Mouse = AbsoluteMousePosition;
#else
			const FVector2f Mouse = LocalMousePosition;
#endif
			float BestDistSquared = TNumericLimits<float>::Max();
			for (int32 Index = 0; Index < Points.Num() - 1; ++Index)
			{
				const FVector2f Closest = FMath::ClosestPointOnSegment2D(Mouse, Points[Index], Points[Index + 1]);
				BestDistSquared = FMath::Min(BestDistSquared, (Mouse - Closest).SizeSquared());
			}

			const float Tolerance = FMath::Max(Params.WireThickness * 2.0f, 10.0f) * ZoomFactor;
			if (BestDistSquared < Tolerance * Tolerance && BestDistSquared < HoverBestDistSquared)
			{
				HoverBestDistSquared = BestDistSquared;
				SplineOverlapResult = FGraphSplineOverlapResult(
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
			if (Length <= KINDA_SMALL_NUMBER || !BubbleImage)
			{
				return;
			}

			const float BubbleSpacing = 64.0f * ZoomFactor;
			const float BubbleSpeed = 192.0f * ZoomFactor;
			const FVector2f BubbleSize = FVector2f(BubbleImage->ImageSize) * ZoomFactor * 0.2f * Params.WireThickness;
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
					DrawElementsList, LayerId, FPaintGeometry(BubblePos, BubbleSize, ZoomFactor),
					BubbleImage, ESlateDrawEffect::None, Params.WireColor);
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
}

/** Node factory that swaps only the wire policy for Blueprint-schema graphs. */
class FUplinkWireNodeFactory : public FGraphNodeFactory
{
public:
	virtual FConnectionDrawingPolicy* CreateConnectionPolicy(const UEdGraphSchema* Schema, int32 InBackLayerID, int32 InFrontLayerID,
		float InZoomFactor, const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements, UEdGraph* InGraphObj) override
	{
		if (CVarUplinkTidyWires.GetValueOnGameThread() != 0 && Schema && Schema->IsA(UEdGraphSchema_K2::StaticClass()))
		{
			return new FUplinkOrthoK2Policy(InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements, InGraphObj);
		}
		return FGraphNodeFactory::CreateConnectionPolicy(
			Schema, InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements, InGraphObj);
	}
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
			return; // panels do not nest
		}
		FChildren* Children = Widget->GetChildren();
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
