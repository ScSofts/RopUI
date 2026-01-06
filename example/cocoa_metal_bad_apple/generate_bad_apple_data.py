#!/usr/bin/env python3
"""
Bad Apple 视频数据生成器

从 MP4 文件提取帧数据，转换为黑白像素格式，用于 Metal 渲染
"""

import cv2
import numpy as np
import sys
import os
from pathlib import Path
import subprocess

def extract_frames(video_path, output_dir="bad_apple_frames", max_frames=None,
                  target_width=None, target_height=None):
    """
    从视频文件中提取帧

    Args:
        video_path: 视频文件路径
        output_dir: 输出目录
        max_frames: 最大帧数限制（可选）
        target_width: 目标宽度（可选，会缩放）
        target_height: 目标高度（可选，会缩放）
    """
    if not os.path.exists(video_path):
        print(f"错误：找不到视频文件 {video_path}")
        return None



    # 打开视频文件
    cap = cv2.VideoCapture(video_path)

    if not cap.isOpened():
        print(f"错误：无法打开视频文件 {video_path}")
        return None

    # 获取视频信息
    fps = cap.get(cv2.CAP_PROP_FPS)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    original_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    original_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    # 确定输出尺寸
    if target_width and target_height:
        width, height = target_width, target_height
        print(f"视频信息：{original_width}x{original_height} -> {width}x{height}, {fps} FPS, 总帧数：{total_frames}")
    else:
        width, height = original_width, original_height
        print(f"视频信息：{width}x{height}, {fps} FPS, 总帧数：{total_frames}")

    frames_data = []
    frame_count = 0

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        if max_frames and frame_count >= max_frames:
            break

        # 如果需要缩放
        if target_width and target_height:
            frame = cv2.resize(frame, (target_width, target_height), interpolation=cv2.INTER_LINEAR)

        # 转换为灰度图
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # 二值化处理（阈值可以调整）
        _, binary = cv2.threshold(gray, 128, 255, cv2.THRESH_BINARY)

        # 转换为适合渲染的格式（0-黑，255-白）
        frame_data = binary.astype(np.uint8)

        frames_data.append(frame_data)

        frame_count += 1

        if frame_count % 100 == 0:
            print(f"已处理 {frame_count} 帧...")

    cap.release()

    print(f"完成！共处理 {len(frames_data)} 帧")
    return frames_data, width, height

def generate_cpp_header(width, height, frame_count, output_file=None):
    """
    生成 C++ 头文件声明（数据从二进制文件加载）

    Args:
        width: 帧宽度
        height: 帧高度
        frame_count: 帧数量
        output_file: 输出文件路径（可选，默认在脚本目录）
    """
    if output_file is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        output_file = os.path.join(script_dir, "bad_apple_data.h")

    with open(output_file, 'w') as f:
        f.write("// Bad Apple 帧数据头文件 - 自动生成\n")
        f.write("// 使用 generate_bad_apple_data.py 从视频生成\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <cstdint>\n")
        f.write("#include <vector>\n")
        f.write("#include <string>\n")
        f.write("#include <fstream>\n\n")

        f.write(f"const int BAD_APPLE_WIDTH = {width};\n")
        f.write(f"const int BAD_APPLE_HEIGHT = {height};\n")
        f.write(f"const int BAD_APPLE_FRAME_COUNT = {frame_count};\n")
        f.write(f"const size_t BAD_APPLE_FRAME_SIZE = {width * height};\n")
        f.write(f"const size_t BAD_APPLE_TOTAL_SIZE = {frame_count * width * height};\n\n")

        # 获取当前脚本目录的绝对路径
        script_dir = os.path.dirname(os.path.abspath(__file__))
        bin_file_path = os.path.join(script_dir, "bad_apple_frames.bin").replace("\\", "/")

        # 生成辅助类
        f.write("class BadAppleData {\n")
        f.write("private:\n")
        f.write("    std::vector<uint8_t> frame_data_;\n")
        f.write("    \n")
        f.write("public:\n")
        f.write("    BadAppleData() {\n")
        f.write("        // 直接使用绝对路径加载数据文件\n")
        f.write(f"        loadData(\"{bin_file_path}\");\n")
        f.write("    }\n")
        f.write("    \n")
        f.write("    bool isLoaded() const { return !frame_data_.empty(); }\n")
        f.write("    \n")
        f.write("    const uint8_t* getFrameData(int frame_index) const {\n")
        f.write("        if (frame_index < 0 || frame_index >= BAD_APPLE_FRAME_COUNT || frame_data_.empty()) {\n")
        f.write("            return nullptr;\n")
        f.write("        }\n")
        f.write("        return &frame_data_[frame_index * BAD_APPLE_FRAME_SIZE];\n")
        f.write("    }\n")
        f.write("    \n")
        f.write("    std::vector<uint8_t> getFrameRGBA(int frame_index) const {\n")
        f.write("        const uint8_t* frame_data = getFrameData(frame_index);\n")
        f.write("        if (!frame_data) return {};\n")
        f.write("        \n")
        f.write("        std::vector<uint8_t> rgba_data(BAD_APPLE_FRAME_SIZE * 4);\n")
        f.write("        for (size_t i = 0; i < BAD_APPLE_FRAME_SIZE; ++i) {\n")
        f.write("            uint8_t pixel = frame_data[i];\n")
        f.write("            rgba_data[i * 4 + 0] = pixel; // R\n")
        f.write("            rgba_data[i * 4 + 1] = pixel; // G\n")
        f.write("            rgba_data[i * 4 + 2] = pixel; // B\n")
        f.write("            rgba_data[i * 4 + 3] = 255;    // A\n")
        f.write("        }\n")
        f.write("        return rgba_data;\n")
        f.write("    }\n")
        f.write("    \n")
        f.write("private:\n")
        f.write("    bool loadData(const std::string& path) {\n")
        f.write("        std::ifstream file(path, std::ios::binary | std::ios::ate);\n")
        f.write("        if (!file.is_open()) {\n")
        f.write("            return false;\n")
        f.write("        }\n")
        f.write("        \n")
        f.write("        std::streamsize size = file.tellg();\n")
        f.write("        file.seekg(0, std::ios::beg);\n")
        f.write("        \n")
        f.write("        if (size != BAD_APPLE_TOTAL_SIZE) {\n")
        f.write("            return false;\n")
        f.write("        }\n")
        f.write("        \n")
        f.write("        frame_data_.resize(BAD_APPLE_TOTAL_SIZE);\n")
        f.write("        file.read(reinterpret_cast<char*>(frame_data_.data()), BAD_APPLE_TOTAL_SIZE);\n")
        f.write("        \n")
        f.write("        return file.good();\n")
        f.write("    }\n")
        f.write("};\n\n")

        # 全局实例
        f.write("// 全局 Bad Apple 数据实例\n")
        f.write("extern BadAppleData g_bad_apple_data;\n")

    print(f"C++ 头文件已生成：{output_file}")

def generate_binary_data(frames_data, output_file=None):
    """
    生成二进制数据文件

    Args:
        frames_data: 帧数据列表
        output_file: 输出文件路径（可选，默认在脚本目录）
    """
    if output_file is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        output_file = os.path.join(script_dir, "bad_apple_frames.bin")

    # 将所有帧数据平铺到一个连续的字节数组中
    all_pixels = []
    for frame in frames_data:
        frame_flat = frame.flatten()
        all_pixels.extend(frame_flat)

    # 转换为 bytes 并写入文件
    with open(output_file, 'wb') as f:
        f.write(bytes(all_pixels))

    print(f"二进制数据文件已生成：{output_file} ({len(all_pixels)} 字节)")

def main():
    if len(sys.argv) < 2:
        print("用法：python generate_bad_apple_data.py <video_file.mp4> [max_frames] [target_width] [target_height]")
        print("示例：python generate_bad_apple_data.py bad_apple.mp4 1000 320 240")
        print("注意：如果不指定尺寸，将使用视频原始尺寸（可能很大）")
        return

    video_path = sys.argv[1]
    max_frames = int(sys.argv[2]) if len(sys.argv) > 2 else None
    target_width = int(sys.argv[3]) if len(sys.argv) > 3 else None
    target_height = int(sys.argv[4]) if len(sys.argv) > 4 else None

    # 提取帧数据
    result = extract_frames(video_path, max_frames=max_frames,
                          target_width=target_width, target_height=target_height)
    if result is None:
        return

    frames_data, width, height = result

    # 检查数据大小
    total_pixels = len(frames_data) * width * height
    total_size_mb = total_pixels / (1024 * 1024)
    print(f"数据统计：{len(frames_data)} 帧, {width}x{height}, 总计 {total_size_mb:.1f} MB")

    if total_size_mb > 100:
        print("警告：数据量很大，可能导致编译时间长和内存占用高")
        response = input("是否继续？(y/N): ")
        if response.lower() != 'y':
            return

    # 生成 C++ 头文件和二进制数据文件
    generate_cpp_header(width, height, len(frames_data))
    generate_binary_data(frames_data)

    print("完成！现在可以在 C++ 代码中使用生成的 bad_apple_data.h 和 bad_apple_frames.bin 文件")

if __name__ == "__main__":
    main()
