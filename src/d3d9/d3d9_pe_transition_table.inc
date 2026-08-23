// Canonical PE recorder transition rows.  This file is consumed by the C++
// algebra header and by scripts/check/gen_pe_transition_table.py; keep the
// spelling deliberately language-neutral so the model and production table
// cannot drift while retaining a small, bounded decision matrix.
DXMT9_PE_STATE_WRITE_ROW(Live, Any, False, Any, QueueDelta, True, True, False, False, True)
DXMT9_PE_STATE_WRITE_ROW(Live, Any, True, False, NoOp, False, False, False, False, False)
DXMT9_PE_STATE_WRITE_ROW(Live, Any, True, True, RetainPending, False, False, False, False, False)
DXMT9_PE_STATE_WRITE_ROW(Recording, PriorValueOperation, Any, Any, ApplyPriorValueOnly, True, False, False, True, AnyNotEqualLive)
DXMT9_PE_STATE_WRITE_ROW(Recording, ExplicitSet, Any, Any, RecordExplicit, False, False, True, False, True)

DXMT9_PE_APPEND_ROW(Prepared, True, False, Accepted, True, False, True, True)
DXMT9_PE_APPEND_ROW(Prepared, False, True, Discarded, False, False, False, True)
DXMT9_PE_APPEND_ROW(Prepared, False, False, Failed, False, True, False, True)
DXMT9_PE_APPEND_ROW(Prepared, True, True, Prepared, False, True, False, False)
DXMT9_PE_APPEND_ROW(Accepted, Any, Any, Accepted, False, False, False, False)
DXMT9_PE_APPEND_ROW(Failed, Any, Any, Failed, False, False, False, False)
DXMT9_PE_APPEND_ROW(Discarded, Any, Any, Discarded, False, False, False, False)
