#include "Box3DReplicationTestSpawner.h"

#include "Box3DAsyncDirector.h"
#include "Box3DAsyncPresentationActor.h"
#include "Box3DIrisReplicationSubsystem.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogBox3DReplicationTest, Log, All);

namespace
{
	// Accepted create commands can still be pending when the spawner is destroyed.
	// Keep the consumer alive until their results are drained and removals are queued.
	struct FCleanup
	{
		TWeakObjectPtr<UBox3DAsyncSubsystem> Physics;
		FBox3DAsyncCreationConsumer Consumer;
		TArray<FBox3DAsyncBodyHandle> Pending;
		TArray<FBox3DAsyncBodyHandle> Remove;

		bool Tick(float)
		{
			UBox3DAsyncSubsystem* Subsystem = Physics.Get();
			if (!Subsystem || !IsValid(Subsystem->GetWorld()) || Subsystem->GetWorld()->bIsTearingDown)
			{
				return false;
			}
			for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
			{
				FBox3DAsyncCreationResult Result;
				if (!Subsystem->TryTakeCreationResult(Consumer, Pending[Index], Result)) continue;
				if (Result.bSucceeded) Remove.Add(Result.BodyHandle);
				Pending.RemoveAtSwap(Index);
			}
			for (int32 Index = Remove.Num() - 1; Index >= 0; --Index)
			{
				if (Subsystem->TryRemoveBody(Remove[Index])) Remove.RemoveAtSwap(Index);
			}
			if (!Pending.IsEmpty() || !Remove.IsEmpty()) return true;
			Subsystem->UnregisterCreationConsumer(Consumer);
			return false;
		}
	};
}

ABox3DReplicationTestSpawner::ABox3DReplicationTestSpawner()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	SetReplicates(false);
	SetReplicateMovement(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded()) VisualMesh = Cube.Object;
}

void ABox3DReplicationTestSpawner::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client) return;
	if (!ABox3DAsyncDirector::FindForWorld(World))
	{
		if (!World->SpawnActor<ABox3DAsyncDirector>())
		{
			UE_LOG(LogBox3DReplicationTest, Error, TEXT("Could not spawn the Box3D director."));
			return;
		}
	}
	if (SpawnDelay <= 0.0f) StartSpawning();
	else World->GetTimerManager().SetTimer(SpawnTimer, this, &ThisClass::StartSpawning, SpawnDelay, false);
}

void ABox3DReplicationTestSpawner::StartSpawning()
{
	UWorld* World = GetWorld();
	UBox3DAsyncSubsystem* Physics = World ? World->GetSubsystem<UBox3DAsyncSubsystem>() : nullptr;
	UBox3DIrisReplicationSubsystem* Replication = World
		                                              ? World->GetSubsystem<UBox3DIrisReplicationSubsystem>()
		                                              : nullptr;
	const FVector HalfExtents = VisualMesh
		                            ? VisualMesh->GetBoundingBox().GetExtent() * VisualScale
		                            : FVector::ZeroVector;
	if (!Physics || !Replication || !VisualMesh || VisualScale.ContainsNaN() || HalfExtents.GetMin() <= 0.0
		|| !FMath::IsFinite(SpawnHeight) || SpawnAreaHalfExtent.ContainsNaN()
		|| SpawnAreaHalfExtent.X < 0.0 || SpawnAreaHalfExtent.Y < 0.0)
	{
		UE_LOG(LogBox3DReplicationTest, Error, TEXT("Invalid Box3D/Iris subsystem, mesh, scale, or spawn area."));
		return;
	}
	if (!Physics->TryRegisterCreationConsumer(CreationConsumer))
	{
		UE_LOG(LogBox3DReplicationTest, Error,
		       TEXT("Could not register Box3D creation consumer; check async settings."));
		return;
	}
	RemainingBodies = FMath::Clamp(BodyCount, 0, 1000);
	NextAdmissionWarningTime = World->GetTimeSeconds() + 5.0;
	bSpawning = true;
	SetActorTickEnabled(true);
	UE_LOG(LogBox3DReplicationTest, Display, TEXT("Spawning %d shared Box3D bodies."), RemainingBodies);
}

void ABox3DReplicationTestSpawner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bSpawning) PumpBodies();
}

bool ABox3DReplicationTestSpawner::ExplodeBox3DBodies(FVector Location, float Radius, float ImpulseStrength,
                                                      int32& AcceptedCount, int32& RejectedCount)
{
	AcceptedCount = 0;
	RejectedCount = 0;
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client || !HasAuthority() || World->bIsTearingDown
		|| Location.ContainsNaN() || !FMath::IsFinite(Radius) || Radius <= 0.0f
		|| !FMath::IsFinite(ImpulseStrength) || ImpulseStrength <= 0.0f || ImpulseStrength > 1.0e9f)
	{
		return false;
	}
	UBox3DAsyncSubsystem* Physics = World->GetSubsystem<UBox3DAsyncSubsystem>();
	FBox3DAsyncPoseFrame Frame;
	if (!Physics || !Physics->TryGetLatestPoseFrame(Frame) || !Frame.World.IsValid()) return false;

	struct FCandidate
	{
		FBox3DAsyncBodyHandle Handle;
		FVector Impulse;
	};
	TArray<FCandidate> Candidates;
	for (const FBox3DAsyncPose& Pose : Frame.Poses)
	{
		const FVector Offset = Pose.Transform.GetLocation() - Location;
		const double Distance = Offset.Length();
		if (!FMath::IsFinite(Distance) || Distance >= Radius) continue;
		const FBox3DAsyncBodyHandle Handle{Frame.World, Pose.BodyId};
		FBox3DAsyncBodyState State;
		if (!Physics->TryGetLatestBodyState(Handle, State)) return false;
		// Box3D assigns zero mass to static and kinematic bodies; they cannot react to impulses.
		if (State.Mass <= 0.0f) continue;
		Candidates.Add({
			Handle, (Distance > 0.0 ? Offset / Distance : FVector::UpVector)
			* (ImpulseStrength * (1.0 - Distance / Radius))
		});
	}
	for (const FCandidate& Candidate : Candidates)
	{
		FBox3DAsyncBodyCommand Command;
		Command.Type = EBox3DAsyncBodyCommandType::ApplyImpulse;
		Command.Value = Candidate.Impulse;
		Command.bWake = true;
		if (Physics->TrySubmitBodyCommand(Candidate.Handle, Command)) ++AcceptedCount;
		else ++RejectedCount;
	}
	return true;
}

void ABox3DReplicationTestSpawner::PumpBodies()
{
	UWorld* World = GetWorld();
	UBox3DAsyncSubsystem* Physics = World ? World->GetSubsystem<UBox3DAsyncSubsystem>() : nullptr;
	UBox3DIrisReplicationSubsystem* Replication = World
		                                              ? World->GetSubsystem<UBox3DIrisReplicationSubsystem>()
		                                              : nullptr;
	if (!Physics || !Replication) return;
	FBox3DAsyncPoseFrame Frame;
	TMap<uint64, FTransform> StandalonePoses;
	if (World->GetNetMode() == NM_Standalone && Physics->TryGetLatestPoseFrame(Frame))
	{
		for (const FBox3DAsyncPose& Pose : Frame.Poses) StandalonePoses.Add(Pose.BodyId, Pose.Transform);
	}
	for (FTestBody& Body : Bodies)
	{
		if (!Body.bCreationComplete)
		{
			FBox3DAsyncCreationResult Result;
			if (!Physics->TryTakeCreationResult(CreationConsumer, Body.Handle, Result)) continue;
			Body.bCreationComplete = true;
			if (!Result.bSucceeded)
			{
				UE_LOG(LogBox3DReplicationTest, Error, TEXT("Box3D body creation failed: %llu"), Body.Handle.Id);
				continue;
			}
			Body.Handle = Result.BodyHandle;
			Replication->RegisterActorlessSharedBody(Body.Handle, Body.Key, FSoftObjectPath(VisualMesh), VisualScale,
			                                         true);
			Body.bRegistered = true;
		}
		if (Body.bRegistered && Frame.World == Body.Handle.World)
		{
			if (const FTransform* Pose = StandalonePoses.Find(Body.Handle.Id)) UpdateStandaloneVisual(Body, *Pose);
		}
	}
	while (RemainingBodies > 0)
	{
		FBox3DAsyncBodyDesc Desc;
		Desc.Motion = EBox3DAsyncBodyMotion::Dynamic;
		const FVector Offset(FMath::FRandRange(-SpawnAreaHalfExtent.X, SpawnAreaHalfExtent.X),
		                     FMath::FRandRange(-SpawnAreaHalfExtent.Y, SpawnAreaHalfExtent.Y), SpawnHeight);
		Desc.Transform = FTransform(FQuat(FRotator(FMath::FRandRange(-180.0f, 180.0f),
		                                           FMath::FRandRange(-180.0f, 180.0f),
		                                           FMath::FRandRange(-180.0f, 180.0f))), GetActorLocation() + Offset);
		FBox3DAsyncBodyShapeDesc& Shape = Desc.Shapes.AddDefaulted_GetRef();
		Shape.Kind = EBox3DAsyncBodyShapeKind::Box;
		Shape.HalfExtents = VisualMesh->GetBoundingBox().GetExtent() * VisualScale;
		Shape.Options.CategoryBits = 1ull;
		Shape.Options.MaskBits = 1ull | 4ull;
		FBox3DAsyncBodyHandle Handle;
		if (!Physics->TryCreateBody(CreationConsumer, Desc, Handle))
		{
			if (World->GetTimeSeconds() >= NextAdmissionWarningTime)
			{
				UE_LOG(LogBox3DReplicationTest, Warning,
				       TEXT("Waiting to create %d bodies; check Box3D capacity and spawn bounds."), RemainingBodies);
				NextAdmissionWarningTime = World->GetTimeSeconds() + 5.0;
			}
			break;
		}
		FTestBody& Body = Bodies.AddDefaulted_GetRef();
		Body.Handle = Handle;
		Body.Key = FGuid::NewGuid();
		--RemainingBodies;
	}
	if (!bReportedCompletion && RemainingBodies == 0)
	{
		int32 Completed = 0;
		int32 Registered = 0;
		for (const FTestBody& Body : Bodies)
		{
			Completed += Body.bCreationComplete ? 1 : 0;
			Registered += Body.bRegistered ? 1 : 0;
		}
		if (Completed == Bodies.Num())
		{
			bReportedCompletion = true;
			UE_LOG(LogBox3DReplicationTest, Display, TEXT("Created %d of %d requested Box3D bodies."), Registered,
			       Bodies.Num());
		}
	}
}

void ABox3DReplicationTestSpawner::UpdateStandaloneVisual(FTestBody& Body, const FTransform& Pose)
{
	UWorld* World = GetWorld();
	if (!World) return;
	FTransform Transform = Pose;
	Transform.SetScale3D(VisualScale);
	ABox3DAsyncPresentationActor* Visual = Body.StandaloneVisual.Get();
	if (!Visual)
	{
		Visual = World->SpawnActorDeferred<ABox3DAsyncPresentationActor>(
			ABox3DAsyncPresentationActor::StaticClass(), Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Visual) return;
		Visual->SetReplicates(false);
		Visual->SetReplicateMovement(false);
		Visual->SetNetworkReplica(true);
		Visual->PresentationMesh->SetStaticMesh(VisualMesh);
		Visual->FinishSpawning(Transform);
		Body.StandaloneVisual = Visual;
	}
	Visual->SetActorTransform(Transform);
}

void ABox3DReplicationTestSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UWorld* World = GetWorld();
	if (World) World->GetTimerManager().ClearTimer(SpawnTimer);
	UBox3DAsyncSubsystem* Physics = World ? World->GetSubsystem<UBox3DAsyncSubsystem>() : nullptr;
	UBox3DIrisReplicationSubsystem* Replication = World
		                                              ? World->GetSubsystem<UBox3DIrisReplicationSubsystem>()
		                                              : nullptr;
	TSharedRef<FCleanup> Cleanup = MakeShared<FCleanup>();
	Cleanup->Physics = Physics;
	Cleanup->Consumer = CreationConsumer;
	for (FTestBody& Body : Bodies)
	{
		if (ABox3DAsyncPresentationActor* Visual = Body.StandaloneVisual.Get()) Visual->Destroy();
		if (Body.bRegistered && Replication) Replication->UnregisterSharedBody(Body.Key, Body.Handle.Id);
		if (!Body.bCreationComplete) Cleanup->Pending.Add(Body.Handle);
		else if (Body.bRegistered) Cleanup->Remove.Add(Body.Handle);
	}
	if (Physics && CreationConsumer.Id != 0 && !Cleanup->Tick(0.0f))
	{
		// Cleanup completed synchronously, or the world is already tearing down.
	}
	else if (Physics && CreationConsumer.Id != 0)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Cleanup](float DeltaSeconds) { return Cleanup->Tick(DeltaSeconds); }));
	}
	Super::EndPlay(EndPlayReason);
}
