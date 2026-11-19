// Fill out your copyright notice in the Description page of Project Settings.


#include "BoxPhysicsExampleGameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/Platform.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

FString ABoxPhysicsExampleGameModeBase::InitNewPlayer(APlayerController* NewPlayerController,
                                                      const FUniqueNetIdRepl& UniqueId, const FString& Options,
                                                      const FString& Portal)
{
	const FString Error = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
	if (!Error.IsEmpty()) return Error;
	const bool bSmokeEnabled = FParse::Param(FCommandLine::Get(), TEXT("Box3DNetSmoke"));
	const FString SmokeRole = bSmokeEnabled
		                          ? UGameplayStatics::ParseOption(Options, TEXT("Box3DSmokeRole"))
		                          : FString();
	if (!SmokeRole.IsEmpty() && NewPlayerController != nullptr
		&& NewPlayerController->PlayerState != nullptr)
	{
		if (SmokeRole.Equals(TEXT("Near"), ESearchCase::IgnoreCase))
		{
			NewPlayerController->PlayerState->SetPlayerName(TEXT("Box3DNear"));
		}
		else if (SmokeRole.Equals(TEXT("Far"), ESearchCase::IgnoreCase))
		{
			NewPlayerController->PlayerState->SetPlayerName(TEXT("Box3DFar"));
		}
		UE_LOG(LogTemp, Display, TEXT("BOX3D_NET_SMOKE_ROLE assigned=%s player=%s"), *SmokeRole,
		       *NewPlayerController->PlayerState->GetPlayerName());
	}
	return Error;
}
