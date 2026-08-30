---- MODULE PeRecorderSemanticProjectionTable ----
(***************************************************************************
 * Generated from DXMT9_PE_SEMANTIC_PRODUCER_TABLE in
 * src/d3d9/d3d9_pe_semantic_tokens.hpp. Do not hand-edit.
 ***************************************************************************)
EXTENDS Naturals, Sequences
SemanticProducerTable == <<
  [producer |-> "DrawPrimitive", record |-> "D9C_COMMAND_RECORD_DRAW_PRIMITIVE", category |-> "Draw"],
  [producer |-> "DrawIndexedPrimitive", record |-> "D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE", category |-> "Draw"],
  [producer |-> "DrawPrimitiveUp", record |-> "D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP", category |-> "DrawUp"],
  [producer |-> "DrawIndexedPrimitiveUp", record |-> "D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP", category |-> "DrawUp"],
  [producer |-> "ApplyState", record |-> "D9C_COMMAND_RECORD_APPLY_STATE", category |-> "StateBlock"],
  [producer |-> "VsFloatConstant", record |-> "D9C_COMMAND_RECORD_SET_VS_CONST_F", category |-> "Constant"],
  [producer |-> "VsIntConstant", record |-> "D9C_COMMAND_RECORD_SET_VS_CONST_I", category |-> "Constant"],
  [producer |-> "VsBoolConstant", record |-> "D9C_COMMAND_RECORD_SET_VS_CONST_B", category |-> "Constant"],
  [producer |-> "PsFloatConstant", record |-> "D9C_COMMAND_RECORD_SET_PS_CONST_F", category |-> "Constant"],
  [producer |-> "PsIntConstant", record |-> "D9C_COMMAND_RECORD_SET_PS_CONST_I", category |-> "Constant"],
  [producer |-> "PsBoolConstant", record |-> "D9C_COMMAND_RECORD_SET_PS_CONST_B", category |-> "Constant"],
  [producer |-> "Clear", record |-> "D9C_COMMAND_RECORD_CLEAR", category |-> "Copy"],
  [producer |-> "StretchRect", record |-> "D9C_COMMAND_RECORD_STRETCH_RECT", category |-> "Copy"],
  [producer |-> "ColorFill", record |-> "D9C_COMMAND_RECORD_COLOR_FILL", category |-> "Copy"],
  [producer |-> "UpdateTexture", record |-> "D9C_COMMAND_RECORD_UPDATE_TEXTURE", category |-> "Update"],
  [producer |-> "UpdateSurface", record |-> "D9C_COMMAND_RECORD_UPDATE_SURFACE", category |-> "Update"],
  [producer |-> "QueryIssue", record |-> "D9C_COMMAND_RECORD_QUERY_ISSUE", category |-> "Query"],
  [producer |-> "Readback", record |-> "D9C_COMMAND_RECORD_READBACK", category |-> "Readback"],
  [producer |-> "ReszDepthResolve", record |-> "D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE", category |-> "Copy"],
  [producer |-> "GenerateMipmaps", record |-> "D9C_COMMAND_RECORD_GENERATE_MIPMAPS", category |-> "Update"],
  [producer |-> "Present", record |-> "D9C_COMMAND_RECORD_PRESENT", category |-> "Present"]
>>
SemanticProducers == {SemanticProducerTable[i].producer : i \in 1..Len(SemanticProducerTable)}
SemanticRecords == {SemanticProducerTable[i].record : i \in 1..Len(SemanticProducerTable)}
SemanticCategories == {SemanticProducerTable[i].category : i \in 1..Len(SemanticProducerTable)}
====
