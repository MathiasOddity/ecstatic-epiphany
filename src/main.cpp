/*
 * Ecstatic Epiphany
 *
 * (c) 2014 Micah Elizabeth Scott
 * http://creativecommons.org/licenses/by/3.0/
 */

#include "lib/camera.h"
#include "narrator.h"

static Narrator narrator;

static void videoCallback(const Camera::VideoChunk &video, void *)
{
    narrator.flow.process(video);
}

int main(int argc, char **argv)
{
    Camera::start(videoCallback);

    narrator.runner.setLayout("layouts/window6x12.json");
    if (!narrator.runner.parseArguments(argc, argv)) {
        return 1;
    }

    narrator.run();
    return 0;
}
