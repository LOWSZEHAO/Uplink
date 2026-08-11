// Copyright (c) 2026 Low Sze Hao. MIT License.
// viewport_screenshot — latent capture of the active viewport (PIE game viewport
// when a session is running, else the editor viewport).

#include "UplinkTools.h"
#include "UplinkCompat.h"
#include "UplinkToolRegistry.h"
#include "UplinkToolUtil.h"

#include "Editor.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"

namespace
{
	struct FCaptureState
	{
		bool bDone = false;
		int32 Width = 0;
		int32 Height = 0;
		TArray<FColor> Pixels;
	};

	class FScreenshotInvocation final : public IUplinkInvocation
	{
	public:
		virtual ~FScreenshotInvocation() override
		{
			if (DelegateHandle.IsValid())
			{
				FScreenshotRequest::OnScreenshotCaptured().Remove(DelegateHandle);
			}
		}

		virtual EUplinkToolStep Start(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			State = MakeShared<FCaptureState>();
			TSharedPtr<FCaptureState> LocalState = State;
			DelegateHandle = FScreenshotRequest::OnScreenshotCaptured().AddLambda(
				[LocalState](int32 Width, int32 Height, const TArray<FColor>& Colors)
				{
					LocalState->Width = Width;
					LocalState->Height = Height;
					LocalState->Pixels = Colors;
					LocalState->bDone = true;
				});

			const bool bRestrictToGame = GEditor != nullptr && GEditor->PlayWorld != nullptr;
			UplinkCompat::RequestViewportScreenshot(bRestrictToGame);

			// The request is served on the next viewport draw; nudge non-realtime
			// editor viewports so one actually happens.
			if (GEditor)
			{
				GEditor->RedrawAllViewports(false);
			}
			return EUplinkToolStep::Pending;
		}

		virtual EUplinkToolStep Tick(const FUplinkToolContext& Ctx, FUplinkToolResult& Out) override
		{
			if (!State->bDone)
			{
				return EUplinkToolStep::Pending;
			}

			FScreenshotRequest::OnScreenshotCaptured().Remove(DelegateHandle);
			DelegateHandle.Reset();

			IImageWrapperModule& ImageWrapperModule =
				FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			const TSharedPtr<IImageWrapper> Png = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
			if (!Png.IsValid() || !Png->SetRaw(
				State->Pixels.GetData(), State->Pixels.Num() * sizeof(FColor),
				State->Width, State->Height, ERGBFormat::BGRA, 8))
			{
				Out = FUplinkToolResult::Error(TEXT("PNG encoding failed"));
				return EUplinkToolStep::Done;
			}

			Out = FUplinkToolResult::Ok();
			Out.Png = Png->GetCompressed(90);
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("width"), State->Width);
			Data->SetNumberField(TEXT("height"), State->Height);
			Out.Data = Data;
			return EUplinkToolStep::Done;
		}

	private:
		TSharedPtr<FCaptureState> State;
		FDelegateHandle DelegateHandle;
	};
}

void UplinkTools::RegisterCapture(FUplinkToolRegistry& Registry)
{
	FUplinkToolInfo Info;
	Info.Name = TEXT("viewport_screenshot");
	Info.Description = TEXT("Capture the active viewport as a PNG image. During PIE this captures the game viewport (what the player sees); otherwise the editor viewport.");
	Info.InputSchema = FUplinkToolRegistry::ParseSchema(
		TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
	Info.bReadOnly = true;
	Info.TimeoutSeconds = 15.0;

	Registry.Register(MoveTemp(Info), []() -> TSharedRef<IUplinkInvocation>
	{
		return MakeShared<FScreenshotInvocation>();
	});
}
