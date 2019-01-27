/************************************************************************

    comb.cpp

    ld-comb-ntsc - NTSC colourisation filter for ld-decode
    Copyright (C) 2018 Chad Page
    Copyright (C) 2018-2019 Simon Inns

    This file is part of ld-decode-tools.

    ld-comb-ntsc is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "comb.h"
#include "../../deemp.h"

// Public methods -----------------------------------------------------------------------------------------------------

Comb::Comb() {
    // Set default configuration
    configuration.blackAndWhite = false;
    configuration.whitePoint100 = false;

    configuration.colorlpf = true; // Use as default
    configuration.colorlpf_hq = true; // Use as default

    // These are the overall dimensions of the input frame
    configuration.fieldWidth = 910;
    configuration.fieldHeight = 263;

    // These are the start and end points for the active video line
    configuration.activeVideoStart = 40;
    configuration.activeVideoEnd = 840;

    // This sets the first visible frame line
    configuration.firstVisibleFrameLine = 43;

    // Set the 16-bit IRE levels
    configuration.blackIre = 15360;
    configuration.whiteIre = 51200;

    // Set the filter type
    configuration.use3D = false;
    configuration.showOpticalFlowMap = false;

    postConfigurationTasks();
}

// Get the comb filter configuration parameters
Comb::Configuration Comb::getConfiguration(void)
{
    return configuration;
}

// Set the comb filter configuration parameters
void Comb::setConfiguration(Comb::Configuration configurationParam)
{
    // Range check the frame dimensions
    if (configuration.fieldWidth > 910) qCritical() << "Comb::Comb(): Frame width exceeds allowed maximum!";
    if (((configuration.fieldHeight * 2) - 1) > 525) qCritical() << "Comb::Comb(): Frame height exceeds allowed maximum!";

    // Range check the video start
    if (configurationParam.activeVideoStart < 16) qCritical() << "Comb::Comb(): activeVideoStart must be > 16!";

    configuration = configurationParam;
    postConfigurationTasks();
}

// Process the input buffer into the RGB output buffer
QByteArray Comb::process(QByteArray firstFieldInputBuffer, QByteArray secondFieldInputBuffer, qreal burstMedianIre,
                         qint32 firstFieldPhaseID, qint32 secondFieldPhaseID)
{
    // Allocate the frame buffer
    FrameBuffer currentFrameBuffer;
    currentFrameBuffer.clpbuffer.resize(3);

    // Allocate the temporary YIQ buffer
    YiqBuffer tempYiqBuffer;

    // Allocate RGB output buffer
    QByteArray rgbOutputBuffer;
    QByteArray bgrBuffer;

    // Interlace the input fields and place in the frame[0]'s raw buffer
    qint32 fieldLine = 0;
    currentFrameBuffer.rawbuffer.clear();
    for (qint32 frameLine = 0; frameLine < frameHeight; frameLine += 2) {
        currentFrameBuffer.rawbuffer.append(firstFieldInputBuffer.mid(fieldLine * configuration.fieldWidth * 2, configuration.fieldWidth * 2));
        currentFrameBuffer.rawbuffer.append(secondFieldInputBuffer.mid(fieldLine * configuration.fieldWidth * 2, configuration.fieldWidth * 2));
        fieldLine++;
    }

    // Set the frames burst median (IRE) - This is used by yiqToRgbFrame to tweak the colour
    // saturation levels (compensating for MTF issues)
    currentFrameBuffer.burstLevel = burstMedianIre;

    // Set the phase IDs for the frame
    currentFrameBuffer.firstFieldPhaseID = firstFieldPhaseID;
    currentFrameBuffer.secondFieldPhaseID = secondFieldPhaseID;

    // 2D or 3D comb filter processing?
    if (!configuration.use3D) {
        // 2D comb filter processing

        // Perform 1D processing
        split1D(&currentFrameBuffer);

        // Perform 2D processing
        split2D(&currentFrameBuffer);

        // Split the IQ values
        splitIQ(&currentFrameBuffer);

        // Copy the current frame to a temporary buffer, so operations on the frame do not
        // alter the original data
        tempYiqBuffer = currentFrameBuffer.yiqBuffer;

        // Process the copy of the current frame
        adjustY(tempYiqBuffer, currentFrameBuffer.firstFieldPhaseID, currentFrameBuffer.secondFieldPhaseID);
        if (configuration.colorlpf) filterIQ(currentFrameBuffer.yiqBuffer);
        doYNR(tempYiqBuffer);
        doCNR(tempYiqBuffer);

        // Convert the YIQ result to RGB
        rgbOutputBuffer = yiqToRgbFrame(tempYiqBuffer, currentFrameBuffer.burstLevel);
    } else {
        // 3D comb filter processing

        // Perform 1D processing
        split1D(&currentFrameBuffer);

        // Perform 2D processing
        split2D(&currentFrameBuffer);

        // Split the IQ values (populates Y)
        splitIQ(&currentFrameBuffer);

        tempYiqBuffer = currentFrameBuffer.yiqBuffer;

        // Process the copy of the current frame (needed for the Y image used by the optical flow)
        adjustY(tempYiqBuffer, currentFrameBuffer.firstFieldPhaseID, currentFrameBuffer.secondFieldPhaseID);
        if (configuration.colorlpf) filterIQ(currentFrameBuffer.yiqBuffer);
        doYNR(tempYiqBuffer);
        doCNR(tempYiqBuffer);

        opticalFlow.denseOpticalFlow(currentFrameBuffer.yiqBuffer, currentFrameBuffer.kValues);

        // Perform 3D processing
        split3D(&currentFrameBuffer, &previousFrameBuffer);

        // Split the IQ values
        splitIQ(&currentFrameBuffer);

        tempYiqBuffer = currentFrameBuffer.yiqBuffer;

        // Process the copy of the current frame (for final output now flow detection has been performed)
        adjustY(tempYiqBuffer, currentFrameBuffer.firstFieldPhaseID, currentFrameBuffer.secondFieldPhaseID);
        if (configuration.colorlpf) filterIQ(currentFrameBuffer.yiqBuffer);
        doYNR(tempYiqBuffer);
        doCNR(tempYiqBuffer);

        // Convert the YIQ result to RGB
        rgbOutputBuffer = yiqToRgbFrame(tempYiqBuffer, currentFrameBuffer.burstLevel);

        // Overlay the optical flow map if required
        if (configuration.showOpticalFlowMap) overlayOpticalFlowMap(currentFrameBuffer, rgbOutputBuffer);

        // Store the current frame
        previousFrameBuffer = currentFrameBuffer;
    }

    // Return the output frame
    return rgbOutputBuffer;
}

// Private methods ----------------------------------------------------------------------------------------------------

// Tasks to be performed if the configuration changes
void Comb::postConfigurationTasks(void)
{
    // Set the IRE scale
    irescale = (configuration.whiteIre - configuration.blackIre) / 100;

    // Set the frame height
    frameHeight = ((configuration.fieldHeight * 2) - 1);
}

// This could do with an explaination of what it is doing...
void Comb::split1D(FrameBuffer *frameBuffer)
{
    bool topInvertphase = false;
    bool bottomInvertphase = false;
    bool invertphase = false;

    if (frameBuffer->firstFieldPhaseID == 2 || frameBuffer->firstFieldPhaseID == 3)
        topInvertphase = true;

    if (frameBuffer->secondFieldPhaseID == 1 || frameBuffer->secondFieldPhaseID == 4)
        bottomInvertphase = true;

    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        // Get a pointer to the line's data
        quint16 *line = reinterpret_cast<quint16 *>(frameBuffer->rawbuffer.data() + (lineNumber * configuration.fieldWidth) * 2);

        // Determine if the line phase should be inverted
        if ((lineNumber % 2) == 0) {
            topInvertphase = !topInvertphase;
            invertphase = topInvertphase;
        } else {
            bottomInvertphase = !bottomInvertphase;
            invertphase = bottomInvertphase;
        }

        Filter f_1di(f_colorlpi);
        Filter f_1dq(f_colorlpq);

        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qint32 phase = h % 4;
            qreal tc1 = (((line[h + 2] + line[h - 2]) / 2) - line[h]);
            qreal tc1f = 0, tsi = 0, tsq = 0;

            if (!invertphase) tc1 = -tc1;

            switch (phase) {
                case 0: tsi = tc1; tc1f = f_1di.feed(tsi); break;
                case 1: tsq = -tc1; tc1f = -f_1dq.feed(tsq); break;
                case 2: tsi = -tc1; tc1f = -f_1di.feed(tsi); break;
                case 3: tsq = tc1; tc1f = f_1dq.feed(tsq); break;
                default: break;
            }

            if (!invertphase) {
                tc1 = -tc1;
                tc1f = -tc1f;
            }

            // Record the 1D C value
            frameBuffer->clpbuffer[0].pixel[lineNumber][h] = tc1;
        }
    }
}

// This could do with an explaination of what it is doing...
void Comb::split2D(FrameBuffer *frameBuffer)
{
    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        qreal *previousLine = frameBuffer->clpbuffer[0].pixel[lineNumber - 2];
        qreal *currentLine = frameBuffer->clpbuffer[0].pixel[lineNumber];
        qreal *nextLine = frameBuffer->clpbuffer[0].pixel[lineNumber + 2];

        // 2D filtering.  can't do top or bottom line - calculated between
        // 1d and 3d because this is filtered
        if ((lineNumber >= 4) && (lineNumber < (frameHeight - 1))) {
            for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
                qreal tc1;

                qreal kp, kn;

                kp  = fabs(fabs(currentLine[h]) - fabs(previousLine[h])); // - fabs(c1line[h] * .20);
                kp += fabs(fabs(currentLine[h - 1]) - fabs(previousLine[h - 1]));
                kp -= (fabs(currentLine[h]) + fabs(currentLine[h - 1])) * .10;
                kn  = fabs(fabs(currentLine[h]) - fabs(nextLine[h])); // - fabs(c1line[h] * .20);
                kn += fabs(fabs(currentLine[h - 1]) - fabs(nextLine[h - 1]));
                kn -= (fabs(currentLine[h]) + fabs(nextLine[h - 1])) * .10;

                kp /= 2;
                kn /= 2;

                qreal p_2drange = 45 * irescale;
                kp = clamp(1 - (kp / p_2drange), 0, 1);
                kn = clamp(1 - (kn / p_2drange), 0, 1);

                qreal sc = 1.0;

                if ((kn > 0) || (kp > 0)) {
                    if (kn > (3 * kp)) kp = 0;
                    else if (kp > (3 * kn)) kn = 0;

                    sc = (2.0 / (kn + kp));// * max(kn * kn, kp * kp);
                    if (sc < 1.0) sc = 1.0;
                } else {
                    if ((fabs(fabs(previousLine[h]) - fabs(nextLine[h])) - fabs((nextLine[h] + previousLine[h]) * .2)) <= 0) {
                        kn = kp = 1;
                    }
                }

                tc1  = ((frameBuffer->clpbuffer[0].pixel[lineNumber][h] - previousLine[h]) * kp * sc);
                tc1 += ((frameBuffer->clpbuffer[0].pixel[lineNumber][h] - nextLine[h]) * kn * sc);
                tc1 /= 8; //(2 * 2);

                // Record the 2D C value
                frameBuffer->clpbuffer[1].pixel[lineNumber][h] = tc1;
            }
        }
    }
}

// This could do with an explaination of what it is doing...
// Only apply 3D processing to stationary pixels
void Comb::split3D(FrameBuffer *currentFrame, FrameBuffer *previousFrame)
{
    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {

        quint16 *currentLine = reinterpret_cast<quint16 *>(currentFrame->rawbuffer.data() + (lineNumber * configuration.fieldWidth) * 2);
        quint16 *previousLine = reinterpret_cast<quint16 *>(previousFrame->rawbuffer.data() + (lineNumber * configuration.fieldWidth) * 2);

        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            currentFrame->clpbuffer[2].pixel[lineNumber][h] = (previousLine[h] - currentLine[h]) / 2;
        }
    }
}

// Spilt the I and Q
void Comb::splitIQ(FrameBuffer *frameBuffer)
{
    bool topInvertphase = false;
    bool bottomInvertphase = false;
    bool invertphase = false;

    if (frameBuffer->firstFieldPhaseID == 2 || frameBuffer->firstFieldPhaseID == 3)
        topInvertphase = true;

    if (frameBuffer->secondFieldPhaseID == 1 || frameBuffer->secondFieldPhaseID == 4)
        bottomInvertphase = true;

    // Clear the target frame YIQ buffer
    frameBuffer->yiqBuffer.clear();

    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        // Get a pointer to the line's data
        quint16 *line = reinterpret_cast<quint16 *>(frameBuffer->rawbuffer.data() + (lineNumber * configuration.fieldWidth) * 2);

        // Determine if the line phase should be inverted
        if ((lineNumber % 2) == 0) {
            topInvertphase = !topInvertphase;
            invertphase = topInvertphase;
        } else {
            bottomInvertphase = !bottomInvertphase;
            invertphase = bottomInvertphase;
        }

        qreal si = 0, sq = 0;
        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qint32 phase = h % 4;

            // Take the 2D C
            qreal cavg = frameBuffer->clpbuffer[1].pixel[lineNumber][h]; // 2D C average

            if (configuration.use3D && frameBuffer->kValues.size() != 0) {
                // The motionK map returns K (0 for stationary pixels to 1 for moving pixels)
                cavg  = frameBuffer->clpbuffer[1].pixel[lineNumber][h] * frameBuffer->kValues[(lineNumber * 910) + h]; // 2D mix
                cavg += frameBuffer->clpbuffer[2].pixel[lineNumber][h] * (1 - frameBuffer->kValues[(lineNumber * 910) + h]); // 3D mix

                // Use only 3D (for testing!)
                //cavg = frameBuffer->clpbuffer[2].pixel[lineNumber][h];
            }

            if (!invertphase) cavg = -cavg;

            switch (phase) {
                case 0: sq = cavg; break;
                case 1: si = -cavg; break;
                case 2: sq = -cavg; break;
                case 3: si = cavg; break;
                default: break;
            }

            frameBuffer->yiqBuffer[lineNumber][h].y = line[h];
            frameBuffer->yiqBuffer[lineNumber][h].i = si;
            frameBuffer->yiqBuffer[lineNumber][h].q = sq;
        }
    }
}

// Filter the IQ from the input YIQ buffer
void Comb::filterIQ(YiqBuffer &yiqBuffer)
{
    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        Filter f_i(configuration.colorlpf_hq ? f_colorlpi : f_colorlpi);
        Filter f_q(configuration.colorlpf_hq ? f_colorlpi : f_colorlpq);

        qint32 qoffset = 2; // f_colorlpf_hq ? f_colorlpi_offset : f_colorlpq_offset;

        qreal filti = 0, filtq = 0;

        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qint32 phase = h % 4;

            switch (phase) {
                case 0: filti = f_i.feed(yiqBuffer[lineNumber][h].i); break;
                case 1: filtq = f_q.feed(yiqBuffer[lineNumber][h].q); break;
                case 2: filti = f_i.feed(yiqBuffer[lineNumber][h].i); break;
                case 3: filtq = f_q.feed(yiqBuffer[lineNumber][h].q); break;
                default: break;
            }

            yiqBuffer[lineNumber][h - qoffset].i = filti;
            yiqBuffer[lineNumber][h - qoffset].q = filtq;
        }
    }
}

// Some kind of noise reduction filter on the C?
void Comb::doCNR(YiqBuffer &yiqBuffer)
{
    Filter f_hpi(f_nrc);
    Filter f_hpq(f_nrc);

    // nr_c is some kind of noise reduction factor (I think)
    qreal nr_c = 0.0 * irescale;

    QVector<YIQ> hplinef;
    hplinef.resize(configuration.fieldWidth + 32);

    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        for (qint32 h = configuration.activeVideoStart; h <= configuration.activeVideoEnd; h++) {
            hplinef[h].i = f_hpi.feed(yiqBuffer[lineNumber][h].i);
            hplinef[h].q = f_hpq.feed(yiqBuffer[lineNumber][h].q);
        }

        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qreal ai = hplinef[h + 12].i;
            qreal aq = hplinef[h + 12].q;

            if (fabs(ai) > nr_c) {
                ai = (ai > 0) ? nr_c : -nr_c;
            }

            if (fabs(aq) > nr_c) {
                aq = (aq > 0) ? nr_c : -nr_c;
            }

            yiqBuffer[lineNumber][h].i -= ai;
            yiqBuffer[lineNumber][h].q -= aq;
        }
    }
}

// Some kind of noise reduction filter on the Y?
void Comb::doYNR(YiqBuffer &yiqBuffer)
{
    Filter f_hpy(f_nr);

    // nr_y is some kind of noise reduction factor (I think)
    qreal nr_y = 1.0 * irescale;

    QVector<YIQ> hplinef;
    hplinef.resize(configuration.fieldWidth + 32);

    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        for (qint32 h = configuration.activeVideoStart; h <= configuration.activeVideoEnd; h++) {
            hplinef[h].y = f_hpy.feed(yiqBuffer[lineNumber][h].y);
        }

        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qreal a = hplinef[h + 12].y;

            if (fabs(a) > nr_y) {
                a = (a > 0) ? nr_y : -nr_y;
            }

            yiqBuffer[lineNumber][h].y -= a;
        }
    }
}

// Convert buffer from YIQ to RGB 16-16-16
QByteArray Comb::yiqToRgbFrame(YiqBuffer yiqBuffer, qreal burstLevel)
{
    QByteArray rgbOutputFrame;
    rgbOutputFrame.resize((configuration.fieldWidth * frameHeight * 3) * 2); // * 3 * 2 for RGB 16-16-16)

    // Initialise the output frame
    rgbOutputFrame.fill(0);

    // Perform YIQ to RGB conversion
    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        // Map the QByteArray data to an unsigned 16 bit pointer
        quint16 *linePointer = reinterpret_cast<quint16 *>(rgbOutputFrame.data() + ((configuration.fieldWidth * 3 * lineNumber) * 2));

        // Offset the output by the activeVideoStart to keep the output frame
        // in the same x position as the input video frame (the +6 realigns the output
        // to the source frame; not sure where the 2 pixel offset is coming from, but
        // it's really not important)
        qint32 o = (configuration.activeVideoStart * 3) + 6;

        // Fill the output frame with the RGB values
        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            RGB rgb(configuration.whiteIre, configuration.blackIre, configuration.whitePoint100, configuration.blackAndWhite);
            YIQ yiq = yiqBuffer[lineNumber][h];

            // Convert YIQ to RGB colour space
            rgb.conv(yiq, burstLevel);

            // Place the RGB values in the output QByteArray
            linePointer[o++] = static_cast<quint16>(rgb.r);
            linePointer[o++] = static_cast<quint16>(rgb.g);
            linePointer[o++] = static_cast<quint16>(rgb.b);
        }
    }

    // Return the RGB frame data
    return rgbOutputFrame;
}

// Convert buffer from YIQ to RGB
void Comb::overlayOpticalFlowMap(FrameBuffer frameBuffer, QByteArray &rgbFrame)
{
    qDebug() << "Comb::overlayOpticalFlowMap(): Overlaying optical flow map onto RGB output";
//    QVector<qreal> motionKMap;
//    opticalFlow.motionK(motionKMap);

    // Overlay the optical flow map on the output RGB
    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        // Map the QByteArray data to an unsigned 16 bit pointer
        quint16 *linePointer = reinterpret_cast<quint16 *>(rgbFrame.data() + ((configuration.fieldWidth * 3 * lineNumber) * 2));

        // Fill the output frame with the RGB values
        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qint32 intensity = static_cast<qint32>(frameBuffer.kValues[(lineNumber * 910) + h] * 65535);
            // Make the RGB more purple to show where motion was detected
            qint32 red = linePointer[(h * 3)] + intensity;
            qint32 green = linePointer[(h * 3) + 2];
            qint32 blue = linePointer[(h * 3) + 2] + intensity;

            if (red > 65535) red = 65535;
            if (green > 65535) green = 65535;
            if (blue > 65535) blue = 65535;

            linePointer[(h * 3)] = static_cast<quint16>(red);
            linePointer[(h * 3) + 1] = static_cast<quint16>(green);
            linePointer[(h * 3) + 2] = static_cast<quint16>(blue);
        }
    }
}

// Remove the colour data from the baseband (Y)
void Comb::adjustY(YiqBuffer &yiqBuffer, qint32 firstFieldPhaseID, qint32 secondFieldPhaseID)
{
    bool topInvertphase = false;
    bool bottomInvertphase = false;
    bool invertphase = false;

    if (firstFieldPhaseID == 2 || firstFieldPhaseID == 3)
        topInvertphase = true;

    if (secondFieldPhaseID == 1 || secondFieldPhaseID == 4)
        bottomInvertphase = true;

    // remove color data from baseband (Y)
    for (qint32 lineNumber = configuration.firstVisibleFrameLine; lineNumber < frameHeight; lineNumber++) {
        // Determine if the line phase should be inverted
        if ((lineNumber % 2) == 0) {
            topInvertphase = !topInvertphase;
            invertphase = topInvertphase;
        } else {
            bottomInvertphase = !bottomInvertphase;
            invertphase = bottomInvertphase;
        }

        for (qint32 h = configuration.activeVideoStart; h < configuration.activeVideoEnd; h++) {
            qreal comp = 0;
            qint32 phase = h % 4;

            YIQ y = yiqBuffer[lineNumber][h + 2];

            switch (phase) {
                case 0: comp = y.q; break;
                case 1: comp = -y.i; break;
                case 2: comp = -y.q; break;
                case 3: comp = y.i; break;
                default: break;
            }

            if (invertphase) comp = -comp;
            y.y += comp;

            yiqBuffer[lineNumber][h + 0] = y;
        }
    }
}

qreal Comb::clamp(qreal v, qreal low, qreal high)
{
        if (v < low) return low;
        else if (v > high) return high;
        else return v;
}

qreal Comb::atan2deg(qreal y, qreal x)
{
    qreal rv = static_cast<double>(atan2(static_cast<long double>(y), x) * (180 / M_PIl));
    if (rv < 0) rv += 360;
    return rv;
}
