input_file = "scream_cut_compressor.pcm"
output_file = "scream_cut_compressor.h"

with open(input_file, "rb") as f:
    data = f.read()

with open(output_file, "w") as f:
    f.write("#pragma once\n\n")
    f.write("#include <stdint.h>\n\n")

    f.write("// Audio format:\n")
    f.write("// 16-bit signed PCM\n")
    f.write("// 16 kHz\n")
    f.write("// Mono\n")
    f.write(f"// {len(data) // 2} samples\n")
    f.write(f"// {len(data)} bytes\n\n")

    f.write("const int16_t audioData[] = {\n")

    for i in range(0, len(data), 2):
        sample = int.from_bytes(data[i:i+2], byteorder="little", signed=True)

        if i % 16 == 0:
            f.write("    ")

        f.write(f"{sample}")

        if i + 2 < len(data):
            f.write(", ")

        if (i // 2 + 1) % 8 == 0:
            f.write("\n")

    f.write("};\n\n")

    f.write("const size_t audioDataLength = ")
    f.write("sizeof(audioData) / sizeof(audioData[0]);\n")