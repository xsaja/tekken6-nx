#pragma once

namespace rex {
class Runtime;
}

namespace tekken6 {
// The 364 retail vtable tail-call thunks (tekken6_vtable_thunks.inc), registered before guest startup.
void InstallVtableThunks(rex::Runtime* runtime);
}  // namespace tekken6
