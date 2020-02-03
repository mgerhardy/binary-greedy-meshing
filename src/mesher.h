#ifndef MESHER_H
#define MESHER_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <stdint.h>
#include "timer.h"

// CS = chunk size (max 62)
static constexpr int CS = 62;
static constexpr int Y_CHUNKS = 16;

// Padded chunk size
static constexpr int CS_P = CS + 2;
static constexpr int CS_P2 = CS_P * CS_P;
static constexpr int CS_P3 = CS_P * CS_P * CS_P;

struct Vertex {
  short px, py, pz;
  uint8_t type;
  uint8_t light;
  uint8_t normal;
};

// MSVC specific ctz
// Use __builtin_ctzll for gcc
inline const int CTZ(uint64_t &x) {
  unsigned long index;
  _BitScanForward64(&index, x);
  return (int)index;
}

inline const int get_axis_i(const int &axis, const int &a, const int &b, const int &c) {
  if (axis == 0) return b + (a * CS_P) + (c * CS_P2);
  else if (axis == 1) return a + (c * CS_P) + (b* CS_P2);
  else return c + (b * CS_P) + (a * CS_P2);
}

// voxels - 64^3 (includes neighboring voxels)
// light_map - ^
std::vector<Vertex>* mesh(std::vector<uint8_t>& voxels, std::vector<uint8_t>& light_map, glm::ivec3 pos) {
  Timer timer("meshing", true);

  uint64_t axis_cols[CS_P2 * 3] = { 0 };
  uint64_t col_face_masks[CS_P2 * 6];

  auto vertices = new std::vector<Vertex>();

  // Step 1: Convert to binary representation for each direction
  auto p = voxels.begin();
  for (int y = 0; y < CS_P; y++) {
    for (int x = 0; x < CS_P; x++) {
      uint64_t zb = 0;
      for (int z = 0; z < CS_P; z++) {
        if (*p > 0) {
          axis_cols[x + (z * CS_P)] |= 1ULL << y;
          axis_cols[z + (y * CS_P) + (CS_P2)] |= 1ULL << x;
          zb |= 1ULL << z;
        }
        p++;
      }
      axis_cols[y + (x * CS_P) + (CS_P2 * 2)] = zb;
    }
  }

  // Step 2: Visible face culling
  for (int axis = 0; axis <= 2; axis++) {
    for (int i = 0; i < CS_P2; i++) {
      uint64_t col = axis_cols[(CS_P2 * axis) + i];
      col_face_masks[(CS_P2 * (axis * 2)) + i] = col & ~((col >> 1) | (1ULL << CS_P - 1));
      col_face_masks[(CS_P2 * (axis * 2 + 1)) + i] = col & ~((col << 1) | 1ULL);
    }
  }

  // world offset
  const short SX = (pos.x * CS) - 1;
  const short SY = (pos.y * CS) - 1;
  const short SZ = (pos.z * CS) - 1;

  // Step 3: Greedy meshing
  for (int face = 0; face < 6; face++) {
    int light_dir = face % 2 == 0 ? 1 : -1;
    int merged_forward[CS_P2] = { 0 };
    for (int forward = 1; forward < CS_P - 1; forward++) {
      uint64_t bits_walking_right = 0;
      int merged_right[CS_P] = { 0 };
      for (int right = 1; right < CS_P - 1; right++) {
        uint64_t bits_here = col_face_masks[right + (forward * CS_P) + (face * CS_P2)];
        uint64_t bits_forward = forward >= CS ? 0 : col_face_masks[right + (forward * CS_P) + (face * CS_P2) + CS_P];
        uint64_t bits_right = right >= CS ? 0 : col_face_masks[right + 1 + (forward * CS_P) + (face * CS_P2)];
        uint64_t bits_merging_forward = bits_here & bits_forward & ~bits_walking_right;
        uint64_t bits_merging_right = bits_here & bits_right;

        uint64_t copy_front = bits_merging_forward;
        while (copy_front) {
          int bit_pos = CTZ(copy_front);
          copy_front &= ~(1ULL << bit_pos);

          if (
            voxels.at(get_axis_i(face / 2, right, forward, bit_pos)) ==
            voxels.at(get_axis_i(face / 2, right, forward + 1, bit_pos)) &&
            light_map.at(get_axis_i(face / 2, right, forward, bit_pos + light_dir)) ==
            light_map.at(get_axis_i(face / 2, right, forward + 1, bit_pos + light_dir))
            ) {
            merged_forward[(right * CS_P) + bit_pos]++;
          }
          else {
            bits_merging_forward &= ~(1ULL << bit_pos);
          }
        }

        uint64_t bits_stopped_forward = bits_here & ~bits_merging_forward;
        while (bits_stopped_forward) {
          int bit_pos = CTZ(bits_stopped_forward);
          bits_stopped_forward &= ~(1ULL << bit_pos);

          // Discards faces from neighbor voxels
          if (bit_pos == 0 || bit_pos == CS_P - 1) continue;

          if (
            (bits_merging_right & (1ULL << bit_pos)) != 0 &&
            merged_forward[(right * CS_P) + bit_pos] == merged_forward[(right + 1) * CS_P + bit_pos] &&
            voxels.at(get_axis_i(face / 2, right, forward, bit_pos)) ==
            voxels.at(get_axis_i(face / 2, right + 1, forward, bit_pos)) &&
            light_map.at(get_axis_i(face / 2, right, forward, bit_pos + light_dir)) ==
            light_map.at(get_axis_i(face / 2, right + 1, forward, bit_pos + light_dir))
            ) {
            bits_walking_right |= 1ULL << bit_pos;
            merged_right[bit_pos]++;
            merged_forward[(right * CS_P) + bit_pos] = 0;
            continue;
          }
          bits_walking_right &= ~(1ULL << bit_pos);

          uint8_t mesh_left = right - merged_right[bit_pos];
          uint8_t mesh_right = right + 1;
          uint8_t mesh_front = forward - merged_forward[(right * CS_P) + bit_pos];
          uint8_t mesh_back = forward + 1;
          uint8_t mesh_up = bit_pos + (face % 2 == 0 ? 1 : 0);

          uint8_t type = voxels.at(get_axis_i(face / 2, right, forward, bit_pos));
          uint8_t light = light_map.at(get_axis_i(face / 2, right, forward, bit_pos + light_dir));

          merged_forward[(right * CS_P) + bit_pos] = 0;
          merged_right[bit_pos] = 0;

          if (face == 0) {
            vertices->insert(vertices->end(), {
              { SX + mesh_left,  SY + mesh_up, SZ + mesh_front, type, light, 0 },
              { SX + mesh_left,  SY + mesh_up, SZ + mesh_back,  type, light, 0 },
              { SX + mesh_right, SY + mesh_up, SZ + mesh_back,  type, light, 0 },
              { SX + mesh_right, SY + mesh_up, SZ + mesh_back,  type, light, 0 },
              { SX + mesh_right, SY + mesh_up, SZ + mesh_front, type, light, 0 },
              { SX + mesh_left,  SY + mesh_up, SZ + mesh_front, type, light, 0 }
            });
          }
          else if (face == 1) {
            vertices->insert(vertices->end(), {
              { SX + mesh_left,  SY + mesh_up, SZ + mesh_back,  type, light, 1 },
              { SX + mesh_left,  SY + mesh_up, SZ + mesh_front, type, light, 1 },
              { SX + mesh_right, SY + mesh_up, SZ + mesh_front, type, light, 1 },
              { SX + mesh_right, SY + mesh_up, SZ + mesh_front, type, light, 1},
              { SX + mesh_right, SY + mesh_up, SZ + mesh_back,  type, light, 1 },
              { SX + mesh_left,  SY + mesh_up, SZ + mesh_back,  type, light, 1 }
            });
          }
          else if (face == 2) {
            vertices->insert(vertices->end(), {
              { SX + mesh_up,  SY + mesh_front, SZ + mesh_left,  type, light, 2 },
              { SX + mesh_up,  SY + mesh_back,  SZ + mesh_left,  type, light, 2 },
              { SX + mesh_up,  SY + mesh_back,  SZ + mesh_right, type, light, 2 },
              { SX + mesh_up,  SY + mesh_back,  SZ + mesh_right, type, light, 2 },
              { SX + mesh_up,  SY + mesh_front, SZ + mesh_right, type, light, 2 },
              { SX + mesh_up,  SY + mesh_front, SZ + mesh_left,  type, light, 2 }
            });
          }
          else if (face == 3) {
            vertices->insert(vertices->end(), {
              { SX + mesh_up,  SY + mesh_back,  SZ + mesh_left,  type, light, 3 },
              { SX + mesh_up,  SY + mesh_front, SZ + mesh_left,  type, light, 3 },
              { SX + mesh_up,  SY + mesh_front, SZ + mesh_right, type, light, 3 },
              { SX + mesh_up,  SY + mesh_front, SZ + mesh_right, type, light, 3 },
              { SX + mesh_up,  SY + mesh_back,  SZ + mesh_right, type, light, 3 },
              { SX + mesh_up,  SY + mesh_back,  SZ + mesh_left,  type, light, 3 }
            });
          }
          else if (face == 4) {
            vertices->insert(vertices->end(), {
              { SX + mesh_front, SY + mesh_left,  SZ + mesh_up, type, light, 4 },
              { SX + mesh_back,  SY + mesh_left,  SZ + mesh_up, type, light, 4 },
              { SX + mesh_back,  SY + mesh_right, SZ + mesh_up, type, light, 4 },
              { SX + mesh_back,  SY + mesh_right, SZ + mesh_up, type, light, 4 },
              { SX + mesh_front, SY + mesh_right, SZ + mesh_up, type, light, 4 },
              { SX + mesh_front, SY + mesh_left,  SZ + mesh_up, type, light, 4 }
            });
          }
          else if (face == 5) {
            vertices->insert(vertices->end(), {
              { SX + mesh_back,  SY + mesh_left,  SZ + mesh_up, type, light, 5 },
              { SX + mesh_front, SY + mesh_left,  SZ + mesh_up, type, light, 5 },
              { SX + mesh_front, SY + mesh_right, SZ + mesh_up, type, light, 5 },
              { SX + mesh_front, SY + mesh_right, SZ + mesh_up, type, light, 5 },
              { SX + mesh_back,  SY + mesh_right, SZ + mesh_up, type, light, 5 },
              { SX + mesh_back,  SY + mesh_left,  SZ + mesh_up, type, light, 5 }
            });
          }
        }
      }
    }
  }

  size_t vertexLength = vertices->size();

  if (vertexLength == 0) {
    delete vertices;
    return nullptr;
  }

  return vertices;
};

#endif