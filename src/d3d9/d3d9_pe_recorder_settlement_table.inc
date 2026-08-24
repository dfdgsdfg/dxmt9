// Canonical composed PE recorder settlement rows. Included by
// d3d9_pe_recorder_settlement.hpp and translated to TLA+ by
// scripts/check/gen_pe_recorder_settlement_table.py.
//
// point, result, action, accepted record, retryable, rollback, poison
DXMT9_PE_RECORDER_SETTLEMENT_ROW(CapacityPre, Succeeded, Continue, False, False, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(CapacityPre, FailedPreEffect, RetryUnattempted, False, True, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(CapacityPre, FailedEffectUnknown, PoisonFailStop, False, False, False, True)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(Emitter, Succeeded, KeepAccepted, True, False, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(Emitter, FailedPreEffect, RollbackEmitter, False, True, True, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(CapacityPost, Succeeded, Continue, True, False, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(CapacityPost, FailedPreEffect, KeepAccepted, True, True, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(CapacityPost, FailedEffectUnknown, PoisonFailStop, True, False, False, True)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(Bridge, Succeeded, Continue, True, False, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(Bridge, FailedPreEffect, RetryUnattempted, False, True, False, False)
DXMT9_PE_RECORDER_SETTLEMENT_ROW(Bridge, FailedEffectUnknown, PoisonFailStop, False, False, False, True)
