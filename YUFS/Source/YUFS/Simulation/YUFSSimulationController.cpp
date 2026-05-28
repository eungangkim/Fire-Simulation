#include "Simulation/YUFSSimulationController.h"

#include "Blueprint/UserWidget.h"
#include "Simulation/YUFSGameInstance.h"
#include "Communication/YUFSEmergencyCommSystem.h"
#include "EngineUtils.h"
#include "Fire/YUFSBinaryManager.h"
#include "Fire/YUFSHeterogeneousVolume.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Level/YUFSLevelDataManager.h"
#include "NPC/YUFSEvacuationNPC.h"
#include "NPC/Behavior/YUFSBehaviorStateMachine.h"

AYUFSSimulationController::AYUFSSimulationController()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AYUFSSimulationController::BeginPlay()
{
	Super::BeginPlay();

	// 씬에서 필요한 액터들 캐싱
	for (TActorIterator<AYUFSBinaryManager> It(GetWorld()); It; ++It)
	{
		BinaryManager = *It;
		break;
	}
	for (TActorIterator<AYUFSEmergencyCommSystem> It(GetWorld()); It; ++It)
	{
		CommSystem = *It;
		break;
	}
	for (TActorIterator<AYUFSHeterogeneousVolume> It(GetWorld()); It; ++It)
	{
		HeterogeneousVolume = *It;
		break;
	}
	for (TActorIterator<AYUFSLevelDataManager> It(GetWorld()); It; ++It)
	{
		CachedLDM = *It;
		break;
	}

	// 씬에 이미 배치된 NPC들 자동 수집
	for (TActorIterator<AYUFSEvacuationNPC> It(GetWorld()); It; ++It)
	{
		RegisterNPC(*It);
	}
	InitialNPCCount = RegisteredNPCs.Num();

	SpawnHUD();

	// 레벨 리로드 후 배치 실험 복원 — GameInstance에 저장된 회차 상태를 읽어옴
	if (UYUFSGameInstance* GI = GetGameInstance<UYUFSGameInstance>())
	{
		if (GI->bHasPendingBatchRun)
		{
			CurrentRunIndex = GI->PendingRunIndex;
			TotalRunCount   = GI->PendingTotalRuns;
			AllRunResults   = GI->AccumulatedResults;
			GI->ClearBatchState();

			UE_LOG(LogTemp, Log, TEXT("[YUFS] Batch resume: Run %d/%d"), CurrentRunIndex, TotalRunCount);
			SetPhase(ESimPhase::FireStartDelay);
		}
	}
}

void AYUFSSimulationController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bIsPaused) return;
	if (CurrentPhase == ESimPhase::WaitingToStart || CurrentPhase == ESimPhase::Completed) return;

	ElapsedSimTime += DeltaTime;

	if (CurrentPhase == ESimPhase::FireStartDelay)
	{
		FirePhaseTimer += DeltaTime;
		if (FirePhaseTimer >= FireStartDelaySeconds)
		{
			SetPhase(ESimPhase::FireActive);
		}
	}
	else if (CurrentPhase == ESimPhase::FireActive)
	{
		TickFireActivePhase(DeltaTime);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 외부 제어 API
// ─────────────────────────────────────────────────────────────────────────────

void AYUFSSimulationController::StartSimulation()
{
	if (CurrentPhase != ESimPhase::WaitingToStart) return;

	CurrentRunIndex = 1;
	bIsPaused = false;
	ElapsedSimTime = 0.f;
	FirePhaseTimer = 0.f;
	bAlarmFired = false;
	bPreRecordedMsgFired = false;
	bLiveAnnouncementFired = false;
	bStaffGuidanceFired = false;
	InitialNPCCount = RegisteredNPCs.Num();
	LiveEvacuatedCount = 0;
	LiveIncapacitatedCount = 0;
	TotalEvacuationTime = 0.f;
	AllRunResults.Empty();

	SetPhase(ESimPhase::FireStartDelay);

	UE_LOG(LogTemp, Log, TEXT("[YUFS] Simulation Run %d/%d started. Fire in %.0f seconds."),
		CurrentRunIndex, TotalRunCount, FireStartDelaySeconds);
}

void AYUFSSimulationController::PauseSimulation()
{
	bIsPaused = true;

	if (HeterogeneousVolume)
	{
		HeterogeneousVolume->PauseFire();
	}

	for (AYUFSEvacuationNPC* NPC : RegisteredNPCs)
	{
		if (!IsValid(NPC)) continue;

		if (UCharacterMovementComponent* MoveComp = NPC->GetCharacterMovement())
		{
			MoveComp->StopMovementImmediately();
			MoveComp->DisableMovement();
		}

		NPC->SetActorTickEnabled(false);
	}

	// 이 줄은 제거
	// UGameplayStatics::SetGlobalTimeDilation(GetWorld(), 0.0001f);
}

void AYUFSSimulationController::ResumeSimulation()
{
	bIsPaused = false;

	if (HeterogeneousVolume && CurrentPhase == ESimPhase::FireActive)
	{
		HeterogeneousVolume->ResumeFire();
	}

	for (AYUFSEvacuationNPC* NPC : RegisteredNPCs)
	{
		if (!IsValid(NPC)) continue;

		NPC->SetActorTickEnabled(true);

		if (UCharacterMovementComponent* MoveComp = NPC->GetCharacterMovement())
		{
			MoveComp->SetMovementMode(MOVE_Walking);
		}
	}

	// 이것도 필요 없음
	// UGameplayStatics::SetGlobalTimeDilation(GetWorld(), 1.f);
}

void AYUFSSimulationController::StopAndResetSimulation()
{
	UE_LOG(LogTemp, Log, TEXT("[YUFS] Simulation STOPPED. Reloading level..."));

	// TimeDilation을 먼저 정상화한 뒤 레벨을 리로드해야 다음 실행 시 정상 속도로 시작됨
	UGameplayStatics::SetGlobalTimeDilation(GetWorld(), 1.f);

	// 레벨 리로드: 파괴된 NPC, 화재 볼륨 상태 등 모든 것을 완전히 초기화
	UGameplayStatics::OpenLevel(GetWorld(), *GetWorld()->GetName());
}

// ─────────────────────────────────────────────────────────────────────────────
// 내부 로직
// ─────────────────────────────────────────────────────────────────────────────

void AYUFSSimulationController::SetPhase(ESimPhase NewPhase)
{
	if (CurrentPhase == NewPhase) return;
	CurrentPhase = NewPhase;
	OnPhaseChanged.Broadcast(NewPhase);

	switch (NewPhase)
	{
	case ESimPhase::FireStartDelay:
		// 화재 전 준비 단계: HeterogeneousVolume을 리셋해두고 정지 상태 유지
		if (HeterogeneousVolume) HeterogeneousVolume->ResetFire();
		UE_LOG(LogTemp, Log, TEXT("[YUFS] Phase: FireStartDelay — Volume reset, waiting %.0f sec."),
			FireStartDelaySeconds);
		break;

	case ESimPhase::FireActive:
		// 화재 시작: HeterogeneousVolume 재생 개시
		if (HeterogeneousVolume) HeterogeneousVolume->StartFire();
		UE_LOG(LogTemp, Warning, TEXT("[YUFS] Phase: FireActive === 🔥 Fire STARTED ==="));
		break;

	case ESimPhase::Completed:
		// 종료: 볼륨 일시정지
		if (HeterogeneousVolume) HeterogeneousVolume->PauseFire();
		UE_LOG(LogTemp, Warning, TEXT("[YUFS] Phase: Completed — Volume paused."));
		break;

	case ESimPhase::WaitingToStart:
		// 완전 리셋
		if (HeterogeneousVolume) HeterogeneousVolume->ResetFire();
		break;

	default:
		break;
	}
}

void AYUFSSimulationController::TickFireActivePhase(float DeltaTime)
{
	const float FireElapsed = ElapsedSimTime - FireStartDelaySeconds;

	// 알람 발령
	if (!bAlarmFired && FireElapsed >= AlarmTriggerOffsetSeconds)
	{
		if (CommSystem)
		{
			CommSystem->ActivateAlarm();
			bAlarmFired = true;
			UE_LOG(LogTemp, Warning, TEXT("[YUFS] === ALARM ACTIVATED ==="));
		}
	}

	// 사전 녹음 방송
	if (!bPreRecordedMsgFired && PreRecordedMsgOffsetSeconds >= 0.f
		&& FireElapsed >= PreRecordedMsgOffsetSeconds)
	{
		if (CommSystem)
		{
			CommSystem->BroadcastPreRecordedMessage();
			bPreRecordedMsgFired = true;
			UE_LOG(LogTemp, Warning, TEXT("[YUFS] === PRE-RECORDED MESSAGE BROADCAST ==="));
		}
	}

	// 실시간 안내 방송
	if (!bLiveAnnouncementFired && LiveAnnouncementOffsetSeconds >= 0.f
		&& FireElapsed >= LiveAnnouncementOffsetSeconds)
	{
		if (CommSystem)
		{
			CommSystem->BroadcastLiveAnnouncement();
			bLiveAnnouncementFired = true;
			UE_LOG(LogTemp, Warning, TEXT("[YUFS] === LIVE ANNOUNCEMENT BROADCAST ==="));
		}
	}

	// 스태프 직접 안내 — 발령 시점의 가장 안전한 출구로 목적지 자동 결정
	if (!bStaffGuidanceFired && StaffGuidanceOffsetSeconds >= 0.f
		&& FireElapsed >= StaffGuidanceOffsetSeconds)
	{
		if (CommSystem && CachedLDM)
		{
			const int32 CurrentFrame = BinaryManager ? BinaryManager->GetCurrentFrame() : 0;
			const FVector SafeExit = CachedLDM->GetNearestSafeExit(
				CommSystem->GetActorLocation(), true, CurrentFrame);
			CommSystem->DispatchStaffGuidance(SafeExit);
			bStaffGuidanceFired = true;
			UE_LOG(LogTemp, Warning, TEXT("[YUFS] === STAFF GUIDANCE DISPATCHED → %s ==="),
				*SafeExit.ToString());
		}
	}

	UpdateLiveCounts();
	CheckCompletionCondition();
	if (CurrentPhase == ESimPhase::Completed)
	{
		return;
	}

	// 최대 시뮬레이션 시간 초과 시 강제 종료
	if (ElapsedSimTime - FireStartDelaySeconds >= MaxSimDurationSeconds)
	{
		UE_LOG(LogTemp, Warning, TEXT("[YUFS] Max simulation duration reached. Forcing end."));
		FinalizeRun();
	}
}

void AYUFSSimulationController::UpdateLiveCounts()
{
	if (!CachedLDM || !BinaryManager) return;

	const int32 CurrentFrame = BinaryManager->GetCurrentFrame();

	// 역방향으로 순회하여 대피완료 NPC를 배열에서 안전하게 제거
	for (int32 i = RegisteredNPCs.Num() - 1; i >= 0; --i)
	{
		AYUFSEvacuationNPC* NPC = RegisteredNPCs[i];
		if (!IsValid(NPC))
		{
			RegisteredNPCs.RemoveAt(i);
			continue;
		}

		UYUFSBehaviorStateMachine* SM = NPC->GetBehaviorStateMachine();
		if (!SM) continue;

		// 행동불능 카운트 (숨은 상태로 유지)
		if (SM->GetCurrentState() == EYUFSBehaviorState::Incapacitated)
		{
			NPC->NotifyEpisodeFinished(EYUFSTerminalReason::Incapacitated);
			LiveIncapacitatedCount++;
			RegisteredNPCs.RemoveAt(i);
			NPC->SetActorHiddenInGame(true);
			NPC->SetActorEnableCollision(false);
			NPC->SetActorTickEnabled(false);
			continue;
		}

		// 출구 도달 여부 확인
		const FVector NearestExit = CachedLDM->GetNearestSafeExit(
			NPC->GetActorLocation(), false, CurrentFrame);
		const float DistToExit = FVector::Distance(NPC->GetActorLocation(), NearestExit);

		if (DistToExit < EvacuationSuccessDistanceCm)
		{
			NPC->NotifyEpisodeFinished(EYUFSTerminalReason::ReachedExit);
			LiveEvacuatedCount++;
			TotalEvacuationTime += ElapsedSimTime;
			RegisteredNPCs.RemoveAt(i);

			UE_LOG(LogTemp, Log, TEXT("[YUFS] NPC '%s' evacuated successfully. Total: %d"),
				*NPC->GetName(), LiveEvacuatedCount);

			// 대피 성공 시 NPC 제거
			NPC->Destroy();
		}
	}
}

void AYUFSSimulationController::CheckCompletionCondition()
{
	// RegisteredNPCs에서 이미 처리된 NPC는 제거되므로,
	// 배열이 비면 모든 NPC가 대피 완료 또는 행동불능 상태
	if (RegisteredNPCs.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[YUFS] All NPCs resolved. Finalizing run %d."), CurrentRunIndex);
		FinalizeRun();
	}
}

void AYUFSSimulationController::FinalizeRun()
{
	if (!RegisteredNPCs.IsEmpty())
	{
		for (AYUFSEvacuationNPC* NPC : RegisteredNPCs)
		{
			if (IsValid(NPC))
			{
				NPC->NotifyEpisodeFinished(EYUFSTerminalReason::TimedOut);
				if (UCharacterMovementComponent* MovementComp = NPC->GetCharacterMovement())
				{
					MovementComp->StopMovementImmediately();
					MovementComp->DisableMovement();
				}
				NPC->SetActorTickEnabled(false);
			}
		}
	}

	SetPhase(ESimPhase::Completed);

	// 결과 요약 구성
	FSimRunResult Result;
	Result.RunIndex = CurrentRunIndex;
	Result.TotalNPCCount = FMath::Max(InitialNPCCount, LiveEvacuatedCount + LiveIncapacitatedCount + RegisteredNPCs.Num());
	Result.EvacuatedCount = LiveEvacuatedCount;
	Result.IncapacitatedCount = LiveIncapacitatedCount;
	Result.EvacuationRate = Result.TotalNPCCount > 0
		? (float)Result.EvacuatedCount / (float)Result.TotalNPCCount
		: 0.f;
	Result.SimDurationSeconds = ElapsedSimTime;
	Result.AverageEvacuationTime = Result.EvacuatedCount > 0
		? TotalEvacuationTime / static_cast<float>(Result.EvacuatedCount)
		: 0.f;

	AllRunResults.Add(Result);
	OnRunCompleted.Broadcast(Result);

	UE_LOG(LogTemp, Warning,
		TEXT("[YUFS] Run %d/%d Done | Evacuated: %d/%d (%.1f%%) | Incapacitated: %d | Duration: %.1fs"),
		CurrentRunIndex, TotalRunCount,
		Result.EvacuatedCount, Result.TotalNPCCount,
		Result.EvacuationRate * 100.f,
		Result.IncapacitatedCount,
		Result.SimDurationSeconds);

	// 배치 실험: 다음 회차 진행
	if (CurrentRunIndex < TotalRunCount)
	{
		FTimerHandle NextRunTimer;
		GetWorld()->GetTimerManager().SetTimer(NextRunTimer, this,
			&AYUFSSimulationController::StartNextRun,
			DelayBetweenRunsSeconds, false);
	}
}

void AYUFSSimulationController::StartNextRun()
{
	// 레벨 리로드 전에 다음 회차 상태를 GameInstance에 보존
	// OpenLevel 이후 이 액터는 파괴되므로 멤버 변수에 저장해봐야 소용 없음
	if (UYUFSGameInstance* GI = GetGameInstance<UYUFSGameInstance>())
	{
		GI->SetupNextRun(CurrentRunIndex + 1, TotalRunCount, AllRunResults);
	}

	UGameplayStatics::SetGlobalTimeDilation(GetWorld(), 1.f);
	UGameplayStatics::OpenLevel(GetWorld(), *GetWorld()->GetName());
}

void AYUFSSimulationController::RegisterNPC(AYUFSEvacuationNPC* NPC)
{
	if (IsValid(NPC) && !RegisteredNPCs.Contains(NPC))
	{
		RegisteredNPCs.Add(NPC);
		if (CurrentPhase == ESimPhase::WaitingToStart)
		{
			InitialNPCCount = RegisteredNPCs.Num();
		}
	}
}

float AYUFSSimulationController::GetFireStartCountdown() const
{
	if (CurrentPhase == ESimPhase::FireStartDelay)
	{
		return FMath::Max(0.f, FireStartDelaySeconds - FirePhaseTimer);
	}
	return 0.f;
}

void AYUFSSimulationController::SpawnHUD()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !HUDWidgetClass) return;

	HUDWidgetInstance = CreateWidget<UUserWidget>(PC, HUDWidgetClass);
	if (HUDWidgetInstance)
	{
		HUDWidgetInstance->AddToViewport();
	}
}
