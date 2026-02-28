# ============================================================================
# synapse/tools/gen_panning_trace.py
# Project Synapse – Synthetic Camera Panning Trace Generator
# ============================================================================
import json

def generate_panning_trace(output_file, atlas_dim=16, view_dim=4, frames=600):
    trace = {"metadata": {"version": "1.1", "generator": "Synapse-PTG"}, "commands": []}
    
    # 1. Register Resources (Tiles 0-255 + Skybox)
    tile_size = 4 * 1024 * 1024  # 4MB
    for i in range(atlas_dim * atlas_dim):
        trace["commands"].append({
            "op": "CREATE_IMAGE", "id": i, "size": tile_size, "mips": 8, "is_tex": True
        })
    skybox_id = 999
    trace["commands"].append({
        "op": "CREATE_IMAGE", "id": skybox_id, "size": 64 * 1024 * 1024, "mips": 1, "is_tex": True
    })

    # 2. Simulation Loop
    x, y = 0, 0
    for f in range(frames):
        # Shift camera every 10 frames
        if f > 0 and f % 10 == 0:
            if x + view_dim < atlas_dim: x += 1
            elif y + view_dim < atlas_dim: x = 0; y += 1

        active_tiles = []
        for dy in range(view_dim):
            for dx in range(view_dim):
                active_tiles.append((y + dy) * atlas_dim + (x + dx))
        
        # Add the persistent skybox
        active_tiles.append(skybox_id)

        # Emit Binding & Draw
        trace["commands"].append({
            "op": "BEGIN_FRAME", "frame": f
        })
        trace["commands"].append({
            "op": "BIND_DESCRIPTOR_SET", "set": 0, "resources": active_tiles
        })
        trace["commands"].append({
            "op": "DRAW", "id": f, "desc_set": 0
        })
        trace["commands"].append({
            "op": "END_FRAME"
        })

    with open(output_file, 'w') as f:
        json.dump(trace, f, indent=2)

if __name__ == "__main__":
    generate_panning_trace("camera_pan_stress.json")