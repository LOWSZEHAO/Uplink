// Copyright 2026 Low Sze Hao. Licensed under the Apache License, Version 2.0.

#include "UplinkModalPump.h"
#include "UplinkTaskManager.h"
#include "UplinkEditorModule.h"

#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpServerModule.h"

namespace UplinkModalPump
{
	namespace
	{
		FUplinkTaskManager* Tasks = nullptr;
		FDelegateHandle ModalTickHandle;
		bool bInPump = false;

		void TickDuringModal(float DeltaTime)
		{
			// The modal loop is a plain while() on the game thread, so a tick
			// that opens another modal nests one loop inside the other and this
			// handler is entered again from underneath itself. The task manager
			// guards its own walk; this guards the HTTP server, which has no
			// such guard and would be pumping a request from inside a request.
			if (bInPump)
			{
				return;
			}
			TGuardValue<bool> Guard(bInPump, true);

			if (FHttpServerModule::IsAvailable())
			{
				// FHttpServerModule::Tick is public but carries no export macro,
				// so it cannot be called by name from another module. It is a
				// virtual override of FTSTickerObjectBase::Tick, and the vtable
				// needs no import - going through the base is the call, not a
				// way around it.
				static_cast<FTSTickerObjectBase&>(FHttpServerModule::Get()).Tick(DeltaTime);
			}

			if (Tasks)
			{
				Tasks->TickTasks(DeltaTime);
			}
		}
	}

	void Initialize(FUplinkTaskManager& InTasks)
	{
		if (!FSlateApplication::IsInitialized() || ModalTickHandle.IsValid())
		{
			return;
		}

		Tasks = &InTasks;
		ModalTickHandle = FSlateApplication::Get().GetOnModalLoopTickEvent()
			.AddStatic(&TickDuringModal);
	}

	void Shutdown()
	{
		if (ModalTickHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(ModalTickHandle);
		}
		ModalTickHandle.Reset();

		// Cleared even when the delegate could not be removed, because the
		// thing this points at is about to be destroyed and a stale tick that
		// finds nullptr is survivable where one that finds freed memory is not.
		Tasks = nullptr;
	}

	bool IsPumping()
	{
		return bInPump;
	}
}
