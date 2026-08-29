#pragma once

class MainWindow;
class ModuleManager;
class RuntimeAgent;

// This application's part of the protocol: cube.* and patch.* commands, the
// events the window and widget produce, the render executor, the patch provider,
// and the application fields in hello.
//
// The agent knows none of it. It keeps the socket, the command table, the two
// refusals, event sequence numbers, history and subscription, and everything
// here reaches it through registerCommand, registerExecutor,
// registerHelloField, registerPatchProvider and publishEvent.
//
// Returns false if any registration was refused, which happens when a name is
// already taken. Nothing is undone: a caller that treats it as fatal at
// startup, which the demo does, never reaches a half-registered agent.
//
// The window and module manager must both outlive the agent. The connections
// made here have the agent as their receiver, so Qt drops them when it is
// destroyed, and the demo's teardown order is what keeps that from happening
// the other way round.
bool registerCubeProtocol(RuntimeAgent& agent, MainWindow& window, ModuleManager& modules);
