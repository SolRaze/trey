#pragma once

// Writes the embedded core sysmodule to /luma/sysmodules when it differs and starts it through
// pm:app unless it already runs. Crash guard: two launches per boot at most, the first start and
// one restart; L+R held skips it. Every outcome goes to the FTP log.
void coreStart();

// Same for the Tailscale sysmodule, when /3ds/relay/tskey exists. ts_node_line (ts_node.h) reads its status.
void tsStart();
