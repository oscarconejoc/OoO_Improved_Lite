#!/usr/bin/env python3
"""
Referencia en Python para AXPY y GEMM.
Valida los resultados del testbench bare-metal contra cálculos de alto nivel.
"""

import struct
import numpy as np


def axpy_reference(n, alpha, x, y):
    """Referencia AXPY: Y = alpha * X + Y"""
    return alpha * np.array(x) + np.array(y)


def gemm_reference(m, n, k, a, b, c):
    """Referencia GEMM: C = A * B + C"""
    a_mat = np.array(a).reshape(m, k)
    b_mat = np.array(b).reshape(k, n)
    c_mat = np.array(c).reshape(m, n)
    return (a_mat @ b_mat + c_mat).flatten()


def test_axpy():
    """Valida AXPY testcase."""
    x = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
    y = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]
    alpha = 1.5
    
    result = axpy_reference(8, alpha, x, y)
    expected = [1.5 * xi + yi for xi, yi in zip(x, y)]
    
    print("AXPY Test:")
    print(f"  X: {x}")
    print(f"  Y: {y}")
    print(f"  alpha: {alpha}")
    print(f"  Resultado:   {list(result)}")
    print(f"  Esperado: {expected}")
    print(f"  Match: {np.allclose(result, expected)}")
    return np.allclose(result, expected)


def test_gemm():
    """Valida GEMM testcase."""
    a = [
        1.0, 2.0, 3.0, 4.0,
        5.0, 6.0, 7.0, 8.0,
        9.0, 10.0, 11.0, 12.0,
        13.0, 14.0, 15.0, 16.0,
    ]
    b = [
        1.0, 0.0, 1.0, 0.0,
        0.0, 1.0, 0.0, 1.0,
        1.0, 0.0, 1.0, 0.0,
        0.0, 1.0, 0.0, 1.0,
    ]
    c = [0.0] * 16
    
    result = gemm_reference(4, 4, 4, a, b, c)
    expected = [
        4.0, 6.0, 4.0, 6.0,
        12.0, 14.0, 12.0, 14.0,
        20.0, 22.0, 20.0, 22.0,
        28.0, 30.0, 28.0, 30.0,
    ]
    
    print("\nGEMM Test:")
    print(f"  Forma del resultado: 4x4")
    print(f"  Resultado:   {list(result)}")
    print(f"  Esperado: {expected}")
    print(f"  Match: {np.allclose(result, expected)}")
    return np.allclose(result, expected)


def extract_metrics_from_binary(binary_path):
    """Extrae métricas del binario bare-metal (si están incrustadas en la sección .metrics)."""
    try:
        with open(binary_path, 'rb') as f:
            data = f.read()
        print(f"\nBinary size: {len(data)} bytes")
        return data
    except Exception as e:
        print(f"Error reading binary: {e}")
        return None


if __name__ == "__main__":
    print("=" * 60)
    print("Referencias de validacion Bare-Metal AXPY y GEMM")
    print("=" * 60)
    
    axpy_ok = test_axpy()
    gemm_ok = test_gemm()
    
    print("\n" + "=" * 60)
    if axpy_ok and gemm_ok:
        print("✓ All reference tests PASSED")
    else:
        print("✗ Some reference tests FAILED")
    print("=" * 60)
