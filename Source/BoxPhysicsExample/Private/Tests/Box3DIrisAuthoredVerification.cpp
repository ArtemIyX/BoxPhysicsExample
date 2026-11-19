#if WITH_DEV_AUTOMATION_TESTS

#include "Box3DAsyncDirector.h"
#include "Box3DAsyncPresentationActor.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	const FGuid SharedFixtureKey(0x19ea556a, 0x4fb14d90, 0x8d3486cf, 0x929193b4);
	const FVector SharedStart(900.0, 400.0, 1000.0);
	// If the layers interact, the private cube lands on top of the shared cube.
	const FVector PrivateStart(900.0, 400.0, 1120.0);
	constexpr double ExpectedSettledZ = 860.0;
	FDelegateHandle AuthoredVerificationHandle;
	bool bTravelIssued = false;
	bool bTestTravel = false;
	bool bTestOriginShift = false;
	bool bTestStreamedLevel = false;
	int32 ExpectedActorlessBodies = INDEX_NONE;

	ABox3DAsyncPresentationActor* SpawnVerificationProp(UWorld& World, UStaticMesh* Mesh, const FTransform& Transform,
		EBox3DAsyncPresentationMode CollisionMode, EBox3DReplicationPolicy Policy, ULevel* Level = nullptr)
	{
		FActorSpawnParameters Spawn;
		Spawn.OverrideLevel = Level;
		Spawn.bDeferConstruction = true;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ABox3DAsyncPresentationActor* Actor = World.SpawnActor<ABox3DAsyncPresentationActor>(
			ABox3DAsyncPresentationActor::StaticClass(), Transform, Spawn);
		if (Actor == nullptr) return nullptr;
		Actor->PresentationMesh->SetStaticMesh(Mesh);
		Actor->CollisionMode = CollisionMode;
		Actor->ReplicationPolicy = Policy;
		if (Policy == EBox3DReplicationPolicy::Shared) Actor->ReplicationKey = SharedFixtureKey;
		Actor->FinishSpawning(Transform);
		return Actor;
	}

	struct FAuthoredVerificationState
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ABox3DAsyncPresentationActor> Shared;
		TWeakObjectPtr<ABox3DAsyncPresentationActor> Private;
		TWeakObjectPtr<ULevelStreamingDynamic> Stream;
		double StartSeconds = 0.0;
		bool bReported = false;
		bool bOriginReported = false;
		bool bOriginShifted = false;
		bool bOriginRestored = false;
		bool bStreamLoadAttempted = false;
		bool bBeforeStreamingReported = false;
		bool bUnloadedReported = false;
		bool bReloadedReported = false;
		FIntVector InitialOrigin = FIntVector::ZeroValue;
	};

	void UpdateStreamVerification(UWorld& World, FAuthoredVerificationState& State, bool bFarClient, double Elapsed)
	{
		const bool bShouldStream = !bFarClient || (Elapsed >= 4.0 && Elapsed < 10.0) || Elapsed >= 13.0;
		if (bShouldStream && !State.bStreamLoadAttempted)
		{
			State.bStreamLoadAttempted = true;
			bool bStarted = false;
			State.Stream = ULevelStreamingDynamic::LoadLevelInstance(&World, TEXT("/Engine/Maps/Entry"),
				FVector::ZeroVector, FRotator::ZeroRotator, bStarted, TEXT("Box3DIrisVerificationStream"));
			if (!bStarted)
			{
				UE_LOG(LogTemp, Error, TEXT("BOX3D_AUTHORED_STREAM_RESULT passed=0 reason=load_failed"));
				return;
			}
		}
		ULevelStreamingDynamic* Stream = State.Stream.Get();
		if (Stream != nullptr)
		{
			Stream->SetShouldBeLoaded(bShouldStream);
			Stream->SetShouldBeVisible(bShouldStream);
			if (bShouldStream && Stream->IsLevelVisible() && !State.Shared.IsValid())
			{
				UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
				State.Shared = SpawnVerificationProp(World, Mesh, FTransform(SharedStart),
					EBox3DAsyncPresentationMode::AuthoredDynamicCollision, EBox3DReplicationPolicy::Shared,
					Stream->GetLoadedLevel());
			}
		}
		if (!bFarClient) return;
		const ABox3DAsyncPresentationActor* Shared = State.Shared.Get();
		const bool bVisible = Stream != nullptr && Stream->IsLevelVisible() && Shared != nullptr && !Shared->IsHidden();
		if (!State.bBeforeStreamingReported && Elapsed >= 2.0)
		{
			State.bBeforeStreamingReported = true;
			UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_STREAM_BEFORE passed=%d"), bVisible ? 0 : 1);
		}
		if (!State.bUnloadedReported && Elapsed >= 11.5)
		{
			State.bUnloadedReported = true;
			UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_STREAM_UNLOADED passed=%d"), bVisible ? 0 : 1);
		}
		if (!State.bReloadedReported && Elapsed >= 17.0)
		{
			State.bReloadedReported = true;
			const bool bAtSettledPose = bVisible && FMath::IsNearlyEqual(Shared->GetActorLocation().Z, ExpectedSettledZ, 12.0);
			int32 NumBindings = 0;
			for (TActorIterator<ABox3DAsyncPresentationActor> It(&World); It; ++It)
			{
				if (It->ReplicationKey == SharedFixtureKey) ++NumBindings;
			}
			UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_STREAM_RESULT passed=%d bindings=%d"),
				bAtSettledPose && NumBindings == 1 ? 1 : 0, NumBindings);
		}
	}
}

static void VerifyBox3DIrisAuthoredSmoke(UWorld* World, ELevelTick, float)
{
	if (World == nullptr || World->WorldType != EWorldType::Game || !World->HasBegunPlay()
		|| !World->GetMapName().EndsWith(TEXT("MAP_AsyncPhysics"))
		|| ABox3DAsyncDirector::FindForWorld(World) == nullptr)
	{
		return;
	}

	static FAuthoredVerificationState State;
	if (State.World.Get() != World)
	{
		State = FAuthoredVerificationState();
		State.World = World;
		State.StartSeconds = FPlatformTime::Seconds();
		State.InitialOrigin = World->OriginLocation;
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (Mesh == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("BOX3D_AUTHORED_RESULT passed=0 reason=missing_fixture_mesh"));
			State.bReported = true;
			return;
		}
		SpawnVerificationProp(*World, Mesh,
			FTransform(FQuat::Identity, FVector(1000.0, 400.0, 800.0), FVector(8.0, 8.0, 0.2)),
			EBox3DAsyncPresentationMode::AuthoredStaticCollision, EBox3DReplicationPolicy::LocalOnly);
		if (!bTestStreamedLevel)
		{
			State.Shared = SpawnVerificationProp(*World, Mesh, FTransform(SharedStart),
				EBox3DAsyncPresentationMode::AuthoredDynamicCollision, EBox3DReplicationPolicy::Shared);
		}
		const ENetMode NetMode = World->GetNetMode();
		const FString Role = World->URL.GetOption(TEXT("Box3DSmokeRole="), TEXT(""));
		if (NetMode == NM_ListenServer || NetMode == NM_Standalone
			|| (NetMode == NM_Client && Role == TEXT("Near")))
		{
			State.Private = SpawnVerificationProp(*World, Mesh, FTransform(PrivateStart),
				EBox3DAsyncPresentationMode::AuthoredDynamicCollision, EBox3DReplicationPolicy::LocalOnly);
		}
	}

	const ENetMode NetMode = World->GetNetMode();
	const double Elapsed = FPlatformTime::Seconds() - State.StartSeconds;
	const bool bNetworkAuthority = NetMode == NM_DedicatedServer || NetMode == NM_ListenServer;
	const FString Role = World->URL.GetOption(TEXT("Box3DSmokeRole="), TEXT(""));
	if (bTestStreamedLevel)
		UpdateStreamVerification(*World, State, NetMode == NM_Client && Role == TEXT("Far"), Elapsed);
	if (bNetworkAuthority && bTestTravel && !bTravelIssued && Elapsed >= 11.0)
	{
		bTravelIssued = true;
		UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_TRAVEL requested=1"));
		World->ServerTravel(TEXT("/Game/Maps/MAP_AsyncPhysics?game=/Script/BoxPhysicsExample.BoxPhysicsExampleGameModeBase"));
		return;
	}
	if (bTestOriginShift && bNetworkAuthority)
	{
		if (!State.bOriginShifted && Elapsed >= 9.0)
		{
			State.bOriginShifted = true;
			const bool bShifted = World->SetNewWorldOrigin(State.InitialOrigin + FIntVector(20000, 0, 0));
			UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_ORIGIN_SHIFT applied=%d"), bShifted ? 1 : 0);
		}
		if (!State.bOriginRestored && Elapsed >= 11.0)
		{
			State.bOriginRestored = true;
			const bool bRestored = World->SetNewWorldOrigin(State.InitialOrigin);
			UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_ORIGIN_RESTORE applied=%d"), bRestored ? 1 : 0);
		}
	}
	if (bTestOriginShift && NetMode == NM_Client && !State.bOriginReported && Elapsed >= 13.0)
	{
		State.bOriginReported = true;
		const ABox3DAsyncPresentationActor* Shared = State.Shared.Get();
		UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_ORIGIN_RESULT passed=%d"),
			Shared != nullptr && Shared->IsHidden() ? 1 : 0);
	}
	if (State.bReported || Elapsed < 7.0) return;
	State.bReported = true;
	const ABox3DAsyncPresentationActor* Shared = State.Shared.Get();
	const ABox3DAsyncPresentationActor* Private = State.Private.Get();
	const bool bExpectsPrivate = NetMode == NM_ListenServer || NetMode == NM_Standalone
		|| (NetMode == NM_Client && Role == TEXT("Near"));
	const double SharedZ = Shared ? Shared->GetActorLocation().Z : -99999.0;
	const double PrivateZ = Private ? Private->GetActorLocation().Z : -99999.0;
	int32 NumSharedBindings = 0;
	int32 NumActorlessVisuals = 0;
	for (TActorIterator<ABox3DAsyncPresentationActor> It(World); It; ++It)
	{
		if (It->ReplicationKey == SharedFixtureKey) ++NumSharedBindings;
		if (It->IsNetworkReplica() && !It->GetIsReplicated() && !It->IsHidden()) ++NumActorlessVisuals;
	}
	const bool bSharedReady = Shared && !Shared->IsHidden() && !Shared->GetIsReplicated() && NumSharedBindings == 1
		&& FMath::IsNearlyEqual(SharedZ, ExpectedSettledZ, 12.0);
	const bool bPrivateReady = bExpectsPrivate
		? Private && !Private->GetIsReplicated() && FMath::IsNearlyEqual(PrivateZ, ExpectedSettledZ, 12.0)
		: !Private;
	if (ExpectedActorlessBodies != INDEX_NONE && bNetworkAuthority)
	{
		const int32 ExpectedVisuals = NetMode == NM_ListenServer ? ExpectedActorlessBodies : 0;
		UE_LOG(LogTemp, Display, TEXT("BOX3D_ACTORLESS_HOST_RESULT passed=%d visuals=%d expected=%d"),
			NumActorlessVisuals == ExpectedVisuals ? 1 : 0, NumActorlessVisuals, ExpectedVisuals);
	}
	UE_LOG(LogTemp, Display, TEXT("BOX3D_AUTHORED_RESULT passed=%d netmode=%d role=%s shared_z=%.2f shared_bindings=%d private=%d private_z=%.2f"),
		bSharedReady && bPrivateReady ? 1 : 0, int32(NetMode), *Role, SharedZ, NumSharedBindings, Private ? 1 : 0, PrivateZ);
}

void StartBox3DIrisAuthoredVerification()
{
	const TCHAR* CommandLine = FCommandLine::Get();
	if (!FParse::Param(CommandLine, TEXT("Box3DNetSmokeAuthored"))) return;
	bTestTravel = FParse::Param(CommandLine, TEXT("Box3DNetSmokeTravel"));
	bTestOriginShift = FParse::Param(CommandLine, TEXT("Box3DNetSmokeServerOriginShift"));
	bTestStreamedLevel = FParse::Param(CommandLine, TEXT("Box3DNetSmokeStreamedLevel"));
	if (FParse::Param(CommandLine, TEXT("Box3DNetSmokeActorlessHost")))
	{
		ExpectedActorlessBodies = 32;
		FParse::Value(CommandLine, TEXT("Box3DNetSmokeBodies="), ExpectedActorlessBodies);
	}
	AuthoredVerificationHandle = FWorldDelegates::OnWorldPostActorTick.AddStatic(&VerifyBox3DIrisAuthoredSmoke);
}

void StopBox3DIrisAuthoredVerification()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(AuthoredVerificationHandle);
}

#endif
