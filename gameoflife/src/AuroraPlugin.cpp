/**
    Copyright 2017 Nanoleaf Ltd.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

    http:www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

    AuroraPlugin.cpp

    Created on: Jul 23, 2017
    Author: Elliot Ford

    Description:
    Beat Detection and Panel Color calculations based on FrequncyStars by Nathan Dyck.
    Each light source on the grid follows the rules to Conway's Game of Life.
    Whenever a beat is detected a new "glider" is randomly spawned at the center of one of the panels.
    each loop calculates the next generation of live cells and removes the dead cells from the grid.
 */


#include "AuroraPlugin.h"
#include "LayoutProcessingUtils.h"
#include "ColorUtils.h"
#include "DataManager.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "Logger.h"
#include "PluginFeatures.h"
#include <stdlib.h>


#ifdef __cplusplus
extern "C" {
#endif

    void initPlugin();
    void getPluginFrame(Frame_t* frames, int* nFrames, int* sleepTime);
    void pluginCleanup();

#ifdef __cplusplus
}
#endif

#define MAX_PALETTE_COLOURS 7   // if more colours then this, we will use just the first this many
#define MAX_CELLS 35   // maxiumum number of cells, might increase this considering a glider takes 5 cells
#define BASE_COLOUR_R 0 // these three settings defined the background colour; set to black
#define BASE_COLOUR_G 0
#define BASE_COLOUR_B 0
#define ADJACENT_PANEL_DISTANCE 86.599995   // hard coded distance between adjacent panels; this ideally should be autodetected
#define TRANSITION_TIME 2  // the transition time to send to panels; set to 100ms currently
#define MINIMUM_INTENSITY 0.2  // the minimum intensity of a source
#define TRIGGER_THRESHOLD 0.5 // used to calculate whether to add a source
#define SPAWN_AMOUNT 1

// Here we store the information accociated with each light source like current
// position, velocity and colour. The information is stored in a list called cells.
typedef struct {
    float x;
    float y;
    int R;
    int G;
    int B;
    bool alive;

} cell_t;

/** Here we store the information accociated with each frequency bin. This
 allows for tracking a degree of historical information.
 */
typedef struct {
    uint32_t latest_minimum;
    uint32_t soundPower;
    int16_t colour;
    uint32_t runningMax;
    uint32_t runningMin;
    uint32_t maximumTrigger;
    uint32_t previousPower;
    uint32_t secondPreviousPower;
} freq_bin;

static RGB_t* paletteColours = NULL; // this is our saved pointer to the colour palette
static int nColours = 0;             // the number of colours in the palette
static LayoutData *layoutData; // this is our saved pointer to the panel layout information
static cell_t cells[MAX_PALETTE_COLOURS*5]; // this is our array for cells
static int ncells = 0;
static freq_bin freq_bins[MAX_PALETTE_COLOURS]; // this is our array for frequency bin historical information.

/**
  * @description: add a value to a running max.
  * @param: runningMax is current runningMax, valueToAdd is added to runningMax, effectiveTrail
  *         defines how many values are effectively tracked. Note this is an approximation.
  * @return: int returned as new runningMax.
  */
int addToRunningMax(int runningMax, int valueToAdd, int effectiveTrail) {
    int trail = effectiveTrail;
    if (valueToAdd > runningMax && effectiveTrail > 1) {
        trail = trail / 2;
    }
    return runningMax - ((float)runningMax / effectiveTrail) + ((float)valueToAdd / trail);
}

/**
 * @description: Initialize the plugin. Called once, when the plugin is loaded.
 * This function can be used to load the LayoutData and the colorPalette from the DataManager.
 * Any allocation, if done here, should be deallocated in the plugin cleanup function
 *
 * @param isSoundPlugin: Setting this flag will indicate that it is a sound plugin, and accordingly
 * sound data will be passed in. If not set, the plugin will be considered an effects plugin
 *
 */
void initPlugin() {

    getColorPalette(&paletteColours, &nColours);  // grab the palette colours and store a pointer to them for later use
    PRINTLOG("The palette has %d colours:\n", nColours);
    if(nColours > MAX_PALETTE_COLOURS) {
        PRINTLOG("There are too many colours in the palette. using only the first %d\n", MAX_PALETTE_COLOURS);
        nColours = MAX_PALETTE_COLOURS;
    }

    for (int i = 0; i < nColours; i++) {
        PRINTLOG("   %d %d %d\n", paletteColours[i].R, paletteColours[i].G, paletteColours[i].B);
    }

    layoutData = getLayoutData(); // grab the layout data and store a pointer to it for later use


    PRINTLOG("The layout has %d panels:\n", layoutData->nPanels);
    for (int i = 0; i < layoutData->nPanels; i++) {
        PRINTLOG("   Id: %d   X, Y: %lf, %lf\n", layoutData->panels[i].panelId,
               layoutData->panels[i].shape->getCentroid().x, layoutData->panels[i].shape->getCentroid().y);
    }

    // here we initialize our freqency bin values so that the plugin starts working reasonably well right away
    for (int i = 0; i < MAX_PALETTE_COLOURS; i++) {
        freq_bins[i].latest_minimum = 0;
        freq_bins[i].runningMax = 3;
        freq_bins[i].maximumTrigger = 1;
    }
    enableFft(nColours);
}



/** Removes a light source from the list of light cells */
void removeSource(int idx)
{
    memmove(cells + idx, cells + idx + 1, sizeof(cell_t) * (ncells - idx - 1));
    ncells--;
}

/** Compute cartesian distance between two points */
float distance(float x1, float y1, float x2, float y2)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    return sqrt(dx * dx + dy * dy);
}

void findDead() {
    int i = 0;
    while(i < ncells){
      if(cells[i].alive == false) {
        removeSource(i);
      } else {
        i++;
      }
    }
}

/**
  * @description: compute the distance from a point to a line
  * @param: x1, y1 and x2, y2 are two points that define the line
  *         x3, y3 is the point
  * @return: dist is the computed distance from the point to the closest point on the line
  *          u is a scalar representing how far along the line do we need to go to get near
  *          to the point
  */
void point2line(float x3, float y3, float x1, float y1, float x2, float y2, float *dist, float *u) // x3,y3 is the point
{
    float px = x2 - x1;
    float py = y2 - y1;
    float magnitude_squared = px * px + py * py;
    *u = ((x3 - x1) * px + (y3 - y1) * py) / magnitude_squared;
    float x = x1 + (*u) * px;
    float y = y1 + (*u) * py;
    float dx = x - x3;
    float dy = y - y3;
    *dist = sqrt(dx * dx + dy * dy);
}

/**
  * @description: Adds a light source to the list of light cells. The light source will have a particular colour
  * and intensity and will move at a particular speed.
*/
void addSource(int paletteIndex, float intensity)
{
    float x;
    float y;
    //int i;

    // we need at least two panels to do anything meaningful in here
    if(layoutData->nPanels < 2) {
        return;
    }
    for(int i = 0; i < SPAWN_AMOUNT; i++){
    // pick a random panel
    int n1;
    //PRINTLOG(n1);
    //int n2;
    //while(1) {
        n1 = drand48() * layoutData->nPanels;
        x = layoutData->panels[n1].shape->getCentroid().x;
        y = layoutData->panels[n1].shape->getCentroid().y;


    // decide in the colour of this light source and factor in the intensity to arrive at an RGB value
    int R = paletteColours[paletteIndex].R;
    int G = paletteColours[paletteIndex].G;
    int B = paletteColours[paletteIndex].B;
    R *= intensity;
    G *= intensity;
    B *= intensity;

    // if we're going to overflow the matrix then kill off cells to make space
    // TODO: we should generate a array of items to add and then make space and add the new items
    if(ncells >= MAX_CELLS) {
      for(int j =0; j < 5; j++) {
        removeSource(0);
      }
    }
    // add all the information to the list of light cells
    //Spawns a Conways game of life gliders
    //TODO: this currently spawns a glider facing one direction, make it so the direction is random
    //TODO: make it so the type of Game of Life item that is spawned is random, Glider, Blinker, Block, etc.
    cells[ncells].x = x;
    cells[ncells].y = y+1;
    cells[ncells].R = (int)R;
    cells[ncells].G = (int)G;
    cells[ncells].B = (int)B;
    cells[ncells].alive = true;
    ncells++;

    cells[ncells].x = x+1;
    cells[ncells].y = y;
    cells[ncells].R = (int)R;
    cells[ncells].G = (int)G;
    cells[ncells].B = (int)B;
    cells[ncells].alive = true;
    ncells++;

    cells[ncells].x = x-1;
    cells[ncells].y = y-1;
    cells[ncells].R = (int)R;
    cells[ncells].G = (int)G;
    cells[ncells].B = (int)B;
    cells[ncells].alive = true;
    ncells++;

    cells[ncells].x = x;
    cells[ncells].y = y-1;
    cells[ncells].R = (int)R;
    cells[ncells].G = (int)G;
    cells[ncells].B = (int)B;
    cells[ncells].alive = true;
    ncells++;

    cells[ncells].x = x+1;
    cells[ncells].y = y-1;
    cells[ncells].R = (int)R;
    cells[ncells].G = (int)G;
    cells[ncells].B = (int)B;
    cells[ncells].alive = true;
    ncells++;
  }
}

/**
  * @description: This function will render the colour of the given single panel given
  * the positions of all the lights in the light source list.
  */
void renderPanel(Panel *panel, int *returnR, int *returnG, int *returnB)
{
    float R = BASE_COLOUR_R;
    float G = BASE_COLOUR_G;
    float B = BASE_COLOUR_B;
    int i;

    // Iterate through all the cells
    // Depending how close the source is to the panel, we take some fraction of its colour and mix it into an
    // accumulator. Newest cells have the most weight. Old cells die away until they are gone.
    for(i = 0; i < ncells; i++) {
        float d = distance(panel->shape->getCentroid().x, panel->shape->getCentroid().y, cells[i].x, cells[i].y);
        d = d / ADJACENT_PANEL_DISTANCE;
        float d2 = d * d;
        float factor = 1.0 / (d2 * 1.5 + 1.0); // determines how much of the source's colour we mix in (depends on distance)
                                               // the formula is not based on physics, it is fudged to get a good effect
                                               // the formula yields a number between 0 and 1
        R = R * (1.0 - factor) + cells[i].R * factor;
        G = G * (1.0 - factor) + cells[i].G * factor;
        B = B * (1.0 - factor) + cells[i].B * factor;
    }
    *returnR = (int)R;
    *returnG = (int)G;
    *returnB = (int)B;
}

void spawn(int x, int y, int R, int G, int B) {
  if(ncells >= MAX_CELLS) {
    removeSource(0);
  }
  // add all the information to the list of light cells
  cells[ncells].x = x;
  cells[ncells].y = y;
  cells[ncells].R = (int)R;
  cells[ncells].G = (int)G;
  cells[ncells].B = (int)B;
  cells[ncells].alive = true;
  ncells++;
}

/**
  * Move the positions of all the light cells based on their velocities. If any particular
  * light source has moved far from the origin then it will be removed from the light source list.
  */
void generateNextGeneration(void)
{
  //Find the overpopulated/underpopulated live cells
  for(int i = 0; i < ncells; i++) {
    int numNeighbors = 0;
    for(int j = 0; j < ncells; j++) {
      if (cells[i].x + 1 == cells[j].x || cells[i].x - 1 == cells[j].x || cells[i].y + 1 == cells[j].y || cells[i].y - 1 == cells[j].y) {
        numNeighbors++;
      /**  for(int k = 0; k < ncells; k++) {
          if((cells[i].x + 1 == cells[j].y + 1 && cells[j].y + 1 == cells[k].x - 1) ||
             (cells[i].x + 1 == cells[j].y + 1 && cells[j].y + 1 == cells[k].y - 1) ||
             (cells[i].x + 1 == cells[j].y - 1 && cells[j].y - 1 == cells[k].x - 1) ||
             (cells[i].x + 1 == cells[j].y - 1 && cells[j].y - 1 == cells[k].x - 1)){
               int averageR = (int)(cells[i].R + cells[j].R + cells[k].R)*.33;
               int averageG = (int)(cells[i].G + cells[j].G + cells[k].G)*.33;
               int averageB = (int)(cells[i].B + cells[j].B + cells[k].B)*.33;
              spawn(cells[i].x + 1, cells[i].y, averageR, averageG, averageB);
            } **/
      }
    }
    if(numNeighbors < 2 || numNeighbors > 3) {
      cells[i].alive = false;
    }
  }

  //Find where to spawn new cells
  cell_t* new_cells;
  for(int i = 0; i < ncells; i++) {
    int numNeighbors = 0;
    for(int j = 0; j < ncells; j++) {
      if (cells[j].x - cells[i].x < 2 && cells[j].x - cells[i].x > 1 && abs(cells[i].y - cells[j].y) < 2 )
    }
  }
}



/**
  * A simple algorithm to detect beats. It finds a strong signal after a period of quietness.
  * Actually, it doesn't detect just beats. For example, classical music often doesn't have
  * strong beats but it has strong instrumental sections. Those would also get detected.
  */
int16_t beat_detector(int i)
{
    int16_t beat_detected = 0;

    //Check for local maximum and if observed, add to running average
    if((freq_bins[i].soundPower + (freq_bins[i].runningMax / 4) < freq_bins[i].previousPower) && (freq_bins[i].previousPower > freq_bins[i].secondPreviousPower)){
        freq_bins[i].runningMax = addToRunningMax(freq_bins[i].runningMax, freq_bins[i].previousPower, 4);
    }

    // update latest minimum.
    if(freq_bins[i].soundPower < freq_bins[i].latest_minimum) {
        freq_bins[i].latest_minimum = freq_bins[i].soundPower;
    }
    else if(freq_bins[i].latest_minimum > 0) {
        freq_bins[i].latest_minimum--;
    }

    // criteria for a "beat"; value must exceed minimum plus a threshold of the runningMax.
    if(freq_bins[i].soundPower > freq_bins[i].latest_minimum + (freq_bins[i].runningMax * TRIGGER_THRESHOLD)) {
        freq_bins[i].latest_minimum = freq_bins[i].soundPower;
        beat_detected = 1;
    }

    // update historical information
    freq_bins[i].secondPreviousPower = freq_bins[i].previousPower;
    freq_bins[i].previousPower = freq_bins[i].soundPower;

    return beat_detected;
}

/**
 * @description: this the 'main' function that gives a frame to the Aurora to display onto the panels
 * If the plugin is an effects plugin the soundFeature buffer will be NULL.
 * If the plugin is a sound visualization plugin, the sleepTime variable will be NULL and is not required to be
 * filled in
 * This function, if is an effects plugin, can specify the interval it is to be called at through the sleepTime variable
 * if its a sound visualization plugin, this function is called at an interval of 50ms or more.
 *
 * @param soundFeature: Carries the processed sound data from the soundModule, NULL if effects plugin
 * @param frames: a pre-allocated buffer of the Frame_t structure to fill up with RGB values to show on panels.
 * Maximum size of this buffer is equal to the number of panels
 * @param nFrames: fill with the number of frames in frames
 * @param sleepTime: specify interval after which this function is called again, NULL if sound visualization plugin
 */
void getPluginFrame(Frame_t* frames, int* nFrames, int* sleepTime) {
    int R;
    int G;
    int B;
    int i;
    uint8_t * fftBins = getFftBins();

#define SKIP_COUNT 50
    static int cnt = 0;
    if (cnt < SKIP_COUNT){
        cnt++;
        return;
    }

    // Compute the sound power (or volume) in each bin
    for(i = 0; i < nColours; i++) {
        freq_bins[i].soundPower = fftBins[i];
        uint8_t beat_detected = beat_detector(i);

        if(beat_detected) {
            if (freq_bins[i].soundPower > freq_bins[i].maximumTrigger) {
                freq_bins[i].maximumTrigger = freq_bins[i].soundPower;
            }

            float intensity = 1.0;

            //calculate an intensity ranging from minimum to 1, using log scale
            if (freq_bins[i].soundPower > 1 && freq_bins[i].runningMax > 1){
                intensity = ((log((float)freq_bins[i].soundPower) / log((float)freq_bins[i].runningMax)) * (1.0 - MINIMUM_INTENSITY)) + MINIMUM_INTENSITY;
            }

            if (intensity > 1.0) {
                intensity = 1.0;
            }

            // add a new light source for each beat detected
            addSource(i, intensity);
        }

    }


    // iterate through all the pals and render each one
    for(i = 0; i < layoutData->nPanels; i++) {
        renderPanel(&layoutData->panels[i], &R, &G, &B);
        frames[i].panelId = layoutData->panels[i].panelId;
        frames[i].r = R;
        frames[i].g = G;
        frames[i].b = B;
        frames[i].transTime = TRANSITION_TIME;
    }

    // move all the light cells so they are ready for the next frame
    generateNextGeneration();
    findDead();

    // this algorithm renders every panel at every frame
    *nFrames = layoutData->nPanels;
}

/**
 * @description: called once when the plugin is being closed.
 * Do all deallocation for memory allocated in initplugin here
 */
void pluginCleanup() {
    // do deallocation here, but there is nothing to deallocate
}
