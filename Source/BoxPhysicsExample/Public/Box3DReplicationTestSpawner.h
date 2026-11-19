#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "World/Box3DAsyncSubsystem.h"
#include "Box3DReplicationTestSpawner.generated.h"

class ABox3DAsyncPresentationActor;
class UStaticMesh;

/** Place in a level with a static Box3D floor to try actorless Iris replication. */
UCLASS()
class BOXPHYSICSEXAMPLE_API ABox3DReplicationTestSpawner : public AActor
{
	GENERATED_BODY()

public:
	ABox3DReplicationTestSpawner();
	virtual void Tick(float DeltaSeconds) override;

	/** Queue radial impulses using completed physics poses. Strength is in kg*cm/s.
	 * Requires body-state publication. Returns false without applying any commands if input,
	 * physics poses, or a candidate's state is unavailable. Counts report queue admission. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Box3D Test")
	bool ExplodeBox3DBodies(FVector Location, float Radius, float ImpulseStrength,
		int32& AcceptedCount, int32& RejectedCount);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditInstanceOnly, Category = "Box3D Test", meta = (ClampMin = "0", ClampMax = "1000"))
	int32 BodyCount = 32;

	UPROPERTY(EditInstanceOnly, Category = "Box3D Test", meta = (ClampMin = "0"))
	float SpawnDelay = 3.0f;

	/** Horizontal half-size of the spawn area, in centimeters. */
	UPROPERTY(EditInstanceOnly, Category = "Box3D Test", meta = (ClampMin = "0"))
	FVector2D SpawnAreaHalfExtent = FVector2D(400.0, 400.0);

	/** Height above this actor, in centimeters. */
	UPROPERTY(EditInstanceOnly, Category = "Box3D Test")
	float SpawnHeight = 500.0f;

	/** Use a centered cube mesh. Physics boxes are sized from its bounding box. */
	UPROPERTY(EditAnywhere, Category = "Box3D Test")
	TObjectPtr<UStaticMesh> VisualMesh;

	UPROPERTY(EditInstanceOnly, Category = "Box3D Test")
	FVector VisualScale = FVector(0.5);

private:
	struct FTestBody
	{
		FBox3DAsyncBodyHandle Handle;
		FGuid Key;
		bool bCreationComplete = false;
		bool bRegistered = false;
		TWeakObjectPtr<ABox3DAsyncPresentationActor> StandaloneVisual;
	};

	void StartSpawning();
	void PumpBodies();
	void UpdateStandaloneVisual(FTestBody& Body, const FTransform& Pose);

	FTimerHandle SpawnTimer;
	FBox3DAsyncCreationConsumer CreationConsumer;
	TArray<FTestBody> Bodies;
	int32 RemainingBodies = 0;
	bool bSpawning = false;
	bool bReportedCompletion = false;
	double NextAdmissionWarningTime = 0.0;
};
