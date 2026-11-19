// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "BoxPhysicsExampleGameModeBase.generated.h"

/**
 * 
 */
UCLASS()
class BOXPHYSICSEXAMPLE_API ABoxPhysicsExampleGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

protected:
	virtual FString InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId,
		const FString& Options, const FString& Portal = TEXT("")) override;
};
