#pragma once

namespace WebServer {

void start();
void stop();
void loop();  // periodic tasks (broadcast telemetry)
bool isRunning();

} // namespace WebServer
