/*
 * Camera Flow Vectorizer - an input device using optical flow
 * vector clustering for low-latency non-location-specific motion
 * capture.
 *
 * Copyright (c) 2014 Micah Elizabeth Scott <micah@scanlime.org>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <opencv2/opencv.hpp>
#include <stdio.h>
#include "camera.h"


class CameraFlowVectorizer {
public:
    CameraFlowVectorizer();

    // Process another chunk of video
    void process(const Camera::VideoChunk &chunk);

    // Overall optical flow estimate
    cv::Point2f overall;

private:
    static constexpr bool kDebug = false;
    static constexpr unsigned kMaxPoints = 50;
    static constexpr unsigned kDecimate = 3;
    static constexpr unsigned kDiscoveryGridSpacing = 6;
    static constexpr unsigned kPointTrialPeriod = 15;     // Number of frames to keep a point before discarding
    static constexpr unsigned kMaxPointAge = 30 * 10;     // Limit stale points; always discard after this age
    static constexpr float kMinPointSpeed = 0.1;          // Minimum speed in pixels/frame to keep a point
    static constexpr float kMinEigThreshold = 0.007;      // Minimum eigenvalue threshold to keep a point

    struct PointInfo {
        float distanceTraveled;
        unsigned age;

        PointInfo() : distanceTraveled(0), age(0) {}
    };

    struct Field {
        cv::Mat frames[2];
        std::vector<cv::Point2f> points;
        std::vector<PointInfo> pointInfo;
        cv::Point2f totalNumerator;
        unsigned totalDenominator;
    };

    Field fields[Camera::kFields];

    void calculateFlow(Field &f);
};


/*****************************************************************************************
 *                                   Implementation
 *****************************************************************************************/


inline CameraFlowVectorizer::CameraFlowVectorizer()
{
    for (unsigned i = 0; i < Camera::kFields; i++) {
        for (unsigned j = 0; j < 2; j++) {
            fields[i].frames[j] = cv::Mat::zeros(Camera::kLinesPerField, Camera::kPixelsPerLine / kDecimate, CV_8UC1);
        }
        fields[i].points.clear();
    }
}

inline void CameraFlowVectorizer::process(const Camera::VideoChunk &chunk)
{
    // Store decimated luminance values only

    Camera::VideoChunk iter = chunk;
    const uint8_t *limit = iter.data + chunk.byteCount;
    const unsigned bytesPerSample = kDecimate * 2;

    // Align to the next stored luminance value
    while ((iter.byteOffset % bytesPerSample) != 1) {
        iter.data++;
        iter.byteOffset++;
    }

    Field &f = fields[iter.field];
    cv::Mat &image = f.frames[1];
    uint8_t *dest = image.data +
        iter.line * (Camera::kPixelsPerLine / kDecimate)
        + iter.byteOffset / bytesPerSample;

    while (iter.data < limit) {
        *(dest++) = *iter.data;
        iter.data += bytesPerSample;
    }

    // End of field?

    if (iter.line == Camera::kLinesPerField - 1 &&
        chunk.byteCount + chunk.byteOffset == Camera::kBytesPerLine) {
        calculateFlow(f);
    }
}

inline void CameraFlowVectorizer::calculateFlow(Field &f)
{            
    /*
     * Each NTSC field has its own independent flow calculator, so that we can react to
     * each field as soon as it arrives without worrying about correlating inter-field motion.
     */

    cv::TermCriteria termcrit(CV_TERMCRIT_ITER|CV_TERMCRIT_EPS, 20, 0.03);
    cv::Size subPixWinSize(6,6), winSize(15,15);

    if (f.points.size() < kMaxPoints) {
        /*
         * Look for more points to track. We specifically want to focus on areas that are moving,
         * as if we were subtracting the background. (Actual background subtraction isn't
         * worth the CPU cost, in our case.) We also want to avoid points too near to any existing
         * ones.
         *
         * To quickly find some interesting points, I further decimate the image into a sparse
         * grid, and look for corners near the grid points that have the most motion.
         */

        cv::Size frameSize = f.frames[0].size();
        int gridWidth = frameSize.width / kDiscoveryGridSpacing;
        int gridHeight = frameSize.height / kDiscoveryGridSpacing;

        std::vector<bool> gridCoverage;
        gridCoverage.resize(gridWidth * gridHeight);
        std::fill(gridCoverage.begin(), gridCoverage.end(), false);

        // Calculate coverage of the discovery grid 
        for (unsigned i = 0; i < f.points.size(); i++) {
            int x = f.points[i].x / kDiscoveryGridSpacing;
            int y = f.points[i].y / kDiscoveryGridSpacing;
            unsigned idx = x + y * gridWidth;
            if (idx < gridCoverage.size()) {
                gridCoverage[idx] = true;
            }
        }

        // Look for the highest-motion point that isn't already on the grid, ignoring image edges.

        cv::Point2f bestPoint = cv::Point2f(0, 0);
        int bestDiff = 0;

        for (int y = 1; y < gridHeight - 1; y++) {
            for (int x = 1; x < gridWidth - 1; x++) {
                if (!gridCoverage[x + y * gridWidth]) {
                    int pixX = x * kDiscoveryGridSpacing;
                    int pixY = y * kDiscoveryGridSpacing;

                    int diff = (int)f.frames[1].at<uint8_t>(pixY, pixX) - (int)f.frames[0].at<uint8_t>(pixY, pixX);
                    int diff2 = diff * diff;

                    if (diff2 > bestDiff) {
                        bestDiff = diff2;
                        bestPoint = cv::Point2f(pixX, pixY);
                    }
                }
            }
        }

        if (bestDiff > 0) {
            // Find a good corner near this point
            std::vector<cv::Point2f> newPoint;
            newPoint.push_back(bestPoint);
            cv::cornerSubPix(f.frames[0], newPoint, subPixWinSize, cv::Size(-1,-1), termcrit);

            // New point
            f.points.push_back(newPoint[0]);
            f.pointInfo.push_back(PointInfo());

            if (kDebug) {
                fprintf(stderr, "flow: Detecting new point (%f, %f) -> (%f, %f). %d points total\n",
                    bestPoint.x, bestPoint.y, newPoint[0].x, newPoint[0].y, f.points.size());
            }
        }
    }

    if (!f.points.empty()) {
        // Run Lucas-Kanade tracker

        std::vector<cv::Point2f> points;
        std::vector<uchar> status;
        std::vector<float> err;

        cv::Point2f numerator = cv::Point2f(0, 0);
        unsigned denominator = 0;

        cv::calcOpticalFlowPyrLK(f.frames[0], f.frames[1], f.points,
            points, status, err, winSize, 3, termcrit, 0, kMinEigThreshold);

        unsigned j = 0;
        for (unsigned i = 0; i < status.size(); i++) {

            if (status[i]) {
                // Point was found; update its info

                PointInfo info = f.pointInfo[i];
                cv::Point2f prevLocation = f.points[i];
                cv::Point2f nextLocation = points[i];
                cv::Point2f motion = nextLocation - prevLocation;
                float distance = sqrtf(motion.x * motion.x + motion.y * motion.y);

                info.age++;
                info.distanceTraveled += distance;

                // Is the point stale? Points that haven't moved will be discarded unless they're in
                // the initial trial period. Points that are too old will be discarded too, so stale
                // unreachable points don't get permanently included.

                if (info.age < kPointTrialPeriod || 
                    (info.age < kMaxPointAge && info.distanceTraveled / info.age > kMinPointSpeed)) {

                    // Store point for next time
                    f.pointInfo[j] = info;
                    f.points[j] = nextLocation;
                    j++;

                    // Add to overall flow vector, using point age as a weight
                    if (info.age > kPointTrialPeriod) {
                        numerator.x += motion.x * info.age;
                        numerator.y += motion.y * info.age;
                        denominator += info.age;
                    }

                } else if (kDebug) {
                    fprintf(stderr, "flow: Forgetting point %d with age=%d distance=%f speed=%f\n",
                        i, info.age, info.distanceTraveled, info.distanceTraveled / info.age);
                }
            } else if (kDebug) {
                fprintf(stderr, "flow: Point %d lost tracking\n", i);
            }
        }

        f.points.resize(j);
        f.pointInfo.resize(j);

        // Update total flow for this field
        f.totalNumerator = numerator;
        f.totalDenominator = denominator;

        // Update overall flow from both fields
        cv::Point2f overallNumerator = fields[0].totalNumerator + fields[1].totalNumerator;
        unsigned overallDenominator = fields[0].totalDenominator + fields[1].totalDenominator;
        overall.x = overallDenominator ? overallNumerator.x / overallDenominator : 0;
        overall.y = overallDenominator ? overallNumerator.y / overallDenominator : 0;

        if (kDebug) {
            fprintf(stderr, "flow: Tracking %d points, overall (%f, %f) denominator=%d\n",
                f.points.size(), overall.x, overall.y, denominator);
        }
    }

    if (kDebug) {
        // Debug screenshots
        static int num = 0;
        if ((++num % 10) == 0) {
            for (unsigned i = 0; i < f.points.size(); ++i) {
                int l = std::min<int>(255, f.pointInfo[i].age);
                cv::circle(f.frames[1], f.points[i], 3, cv::Scalar(l));
            }

            char fn[256];
            snprintf(fn, sizeof fn, "frame-%04d.png", num);
            cv::imwrite(fn, f.frames[1]);
        }
    }

    std::swap(f.frames[0], f.frames[1]);
}
