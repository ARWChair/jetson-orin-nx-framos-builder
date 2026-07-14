#pragma once
// Minimaler Parser fuer das bekannte nvdewarper-INI-Format, z.B.:
//
// [property]
// output-width=1552
// output-height=1552
// [surface0]
// projection-type=2
// surface-index=0
// width=2064
// height=1552
// focal-length=835
// top-angle=38.0
// bottom-angle=-38.0
// pitch=0.0
// yaw=0.0
// roll=0.0
//
// Wir nutzen nur die Felder, die fuer die Perspective-Rueckprojektion
// (Fisheye -> rektilinear) noetig sind. projection-type wird aktuell nur
// fuer "2" (Perspective) unterstuetzt -- fuer PushBroom/VertRadCyl muesste
// der Shader erweitert werden.

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <cmath>
#include <stdexcept>
#include <algorithm>

struct DewarpConfig {
    // [property]
    int outputWidth  = 1552;
    int outputHeight = 1552;

    // [surface0]
    int    projectionType = 2;
    int    inputWidth     = 2064;
    int    inputHeight    = 1552;
    double focalLength    = 835.0;
    double topAngleDeg    = 38.0;
    double bottomAngleDeg = -38.0;
    double pitchDeg       = 0.0;
    double yawDeg         = 0.0;
    double rollDeg        = 0.0;

    static std::string trim(const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static DewarpConfig loadFromFile(const std::string &path) {
        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Kann Dewarp-Config nicht oeffnen: " + path);

        DewarpConfig cfg;
        std::string line, section;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);

            try {
                if (section == "property") {
                    if (key == "output-width")  cfg.outputWidth  = std::stoi(val);
                    if (key == "output-height") cfg.outputHeight = std::stoi(val);
                } else if (section.rfind("surface", 0) == 0) {
                    if (key == "projection-type") cfg.projectionType = std::stoi(val);
                    if (key == "width")           cfg.inputWidth     = std::stoi(val);
                    if (key == "height")          cfg.inputHeight    = std::stoi(val);
                    if (key == "focal-length")    cfg.focalLength    = std::stod(val);
                    if (key == "top-angle")       cfg.topAngleDeg    = std::stod(val);
                    if (key == "bottom-angle")    cfg.bottomAngleDeg = std::stod(val);
                    if (key == "pitch")           cfg.pitchDeg       = std::stod(val);
                    if (key == "yaw")             cfg.yawDeg         = std::stod(val);
                    if (key == "roll")            cfg.rollDeg        = std::stod(val);
                }
            } catch (...) {
                // Zeile ignorieren, falls unerwartetes Format
            }
        }
        return cfg;
    }

    // Baut die 3x3 Rotationsmatrix aus yaw/pitch/roll (Grad).
    // Konvention: R = Rz(roll) * Rx(pitch) * Ry(yaw), angewandt auf den
    // Ausgabestrahl bevor er ins Fisheye-Modell zurueckprojiziert wird.
    //
    // WICHTIG: GLSL-mat3-Uniforms erwarten die Daten column-major.
    // glUniformMatrix3fv() wird in main.cpp mit transpose=GL_FALSE
    // aufgerufen (transpose=GL_TRUE ist unter GLES3/OpenGL ES nicht
    // erlaubt), d.h. wir muessen hier selbst schon column-major
    // schreiben: outMat3[col*3 + row] = R(row, col).
    // Bei pitch=yaw=roll=0 ist R die Identitaet, der Fehler war also
    // mit der Default-Config unsichtbar -- sobald man die Kamera
    // tatsaechlich verdreht, hat man sonst die falsche Rotationsrichtung.
    void buildRotationMatrix(float outMat3[9]) const {
        double y = yawDeg   * M_PI / 180.0;
        double p = pitchDeg * M_PI / 180.0;
        double r = rollDeg  * M_PI / 180.0;

        double cy = cos(y), sy = sin(y);
        double cp = cos(p), sp = sin(p);
        double cr = cos(r), sr = sin(r);

        // Alle Teilmatrizen row-major: M[row*3+col]
        double Ry[9] = {
             cy, 0, sy,
              0, 1,  0,
            -sy, 0, cy
        };
        double Rx[9] = {
            1,  0,   0,
            0, cp, -sp,
            0, sp,  cp
        };
        double Rz[9] = {
            cr, -sr, 0,
            sr,  cr, 0,
             0,   0, 1
        };

        auto matmul = [](const double A[9], const double B[9], double C[9]) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    double s = 0;
                    for (int k = 0; k < 3; k++) s += A[i*3+k] * B[k*3+j];
                    C[i*3+j] = s;
                }
        };

        double tmp[9], result[9];
        matmul(Rx, Ry, tmp);
        matmul(Rz, tmp, result);   // result[row*3+col] = R(row, col)

        // Transponiert (column-major) rausschreiben, damit GLSL mit
        // transpose=GL_FALSE die richtige Matrix bekommt.
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                outMat3[col*3 + row] = static_cast<float>(result[row*3 + col]);
    }

    double verticalFovRad() const {
        return (topAngleDeg - bottomAngleDeg) * M_PI / 180.0;
    }
};
