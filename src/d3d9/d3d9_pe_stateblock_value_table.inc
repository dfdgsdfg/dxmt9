// Qualified StateBlock value disposition rows. Lifecycle legality remains in
// d3d9_pe_stateblock_transition_table.inc; these rows bind value publication.
//
// event, action, preserve tracked set, refresh snapshot, publish live, poison
DXMT9_PE_STATEBLOCK_VALUE_ROW(CapturePreEffectFailed, Preserve, True, False, False, False)
DXMT9_PE_STATEBLOCK_VALUE_ROW(CaptureBackendFailed, PoisonFailStop, True, False, False, True)
DXMT9_PE_STATEBLOCK_VALUE_ROW(CaptureAccepted, PublishCapture, True, True, False, False)
DXMT9_PE_STATEBLOCK_VALUE_ROW(ApplyPreEffectFailed, Preserve, True, False, False, False)
DXMT9_PE_STATEBLOCK_VALUE_ROW(ApplyPrepared, Preserve, True, False, False, False)
DXMT9_PE_STATEBLOCK_VALUE_ROW(ApplyBackendFailed, PoisonFailStop, True, False, False, True)
DXMT9_PE_STATEBLOCK_VALUE_ROW(ApplyAccepted, PublishApply, True, False, True, False)
