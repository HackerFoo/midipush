#include <cstdlib>
#include <iostream>
#include <memory>

#include <rtmidi/RtMidi.h>
#include <signal.h>

void mycallback(double deltatime, std::vector<unsigned char> *message,
                void *userData) {
  unsigned int nBytes = message->size();
  for (unsigned int i = 0; i < nBytes; i++)
    std::cout << "Byte " << i << " = " << (int)message->at(i) << ", ";
  if (nBytes > 0)
    std::cout << "stamp = " << deltatime << std::endl;
}

int main() {
  auto midiin = std::make_unique<RtMidiIn>();

  // Check available ports.
  unsigned int nPorts = midiin->getPortCount();
  if (nPorts == 0) {
    std::cout << "No ports available!\n";
    return 0;
  }
  midiin->openPort(nPorts-1);

  // Set our callback function.  This should be done immediately after
  // opening the port to avoid having incoming messages written to the
  // queue.
  midiin->setCallback(&mycallback);

  // Don't ignore sysex, timing, or active sensing messages.
  midiin->ignoreTypes(false, false, false);

  std::cout << "\nReading MIDI input ... press <enter> to quit.\n";
  char input;
  std::cin.get(input);

  return 0;
}