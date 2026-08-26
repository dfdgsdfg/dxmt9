#!/usr/bin/env bash

# Canonical 3DMark scene and suite selectors shared by the 05 and 06 launchers.
# The caller supplies the product, requested preset, and optional complete raw
# argument override. Results are returned through DXMT_3DMARK_RESOLVED_*.

dxmt_resolve_3dmark_lane() {
  local product=$1
  local requested_lane=$2
  local raw_args=$3
  local selection_args=""

  if [[ -n "$raw_args" ]]; then
    DXMT_3DMARK_RESOLVED_LANE="custom"
    DXMT_3DMARK_RESOLVED_SOURCE="args"
    DXMT_3DMARK_RESOLVED_SELECTION=""
    DXMT_3DMARK_RESOLVED_ARGS="$raw_args"
    return 0
  fi

  case "$product:$requested_lane" in
    05:gt1|06:gt1) selection_args="-gt1" ;;
    05:gt2|06:gt2) selection_args="-gt2" ;;
    05:gt3) selection_args="-gt3" ;;
    06:sm2) selection_args="-gt1 -gt2" ;;
    06:hdr1) selection_args="-hdr1" ;;
    06:hdr2) selection_args="-hdr2" ;;
    06:hdr) selection_args="-hdr1 -hdr2" ;;
    05:graphics|06:graphics) selection_args="-gtall" ;;
    05:cpu1|06:cpu1) selection_args="-cpu1" ;;
    05:cpu2|06:cpu2) selection_args="-cpu2" ;;
    05:cpu|06:cpu) selection_args="-cpuall" ;;
    05:score|06:score) selection_args="-gtall -cpuall" ;;
    05:feature|06:feature) selection_args="-featureall" ;;
    05:batch|06:batch) selection_args="-batchall" ;;
    05:all|06:all)
      selection_args="-gtall -cpuall -featureall -batchall"
      ;;
    *)
      if [[ "$product" == "05" ]]; then
        echo "unknown 3DMark05 lane '$requested_lane'; expected one of: gt1 gt2 gt3 graphics cpu1 cpu2 cpu score feature batch all" >&2
      elif [[ "$product" == "06" ]]; then
        echo "unknown 3DMark06 lane '$requested_lane'; expected one of: gt1 gt2 sm2 hdr1 hdr2 hdr graphics cpu1 cpu2 cpu score feature batch all" >&2
      else
        echo "unknown 3DMark product '$product'" >&2
      fi
      return 2
      ;;
  esac

  DXMT_3DMARK_RESOLVED_LANE="$requested_lane"
  DXMT_3DMARK_RESOLVED_SOURCE="preset"
  DXMT_3DMARK_RESOLVED_SELECTION="$selection_args"
  DXMT_3DMARK_RESOLVED_ARGS="$selection_args -nosplash -nosysteminfo -noscreens"
}

dxmt_print_3dmark_lane_identity() {
  local product=$1
  echo "[3dmark-lane] product=$product lane=$DXMT_3DMARK_RESOLVED_LANE source=$DXMT_3DMARK_RESOLVED_SOURCE"
}
