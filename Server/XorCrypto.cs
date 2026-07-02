// Server/XorCrypto.cs
using System.Text;

namespace TitanRAT.Server;

public static class XorCrypto
{
    private const byte Key = 0xAA;
    
    public static byte[] XorBytes(byte[] data)
    {
        var result = new byte[data.Length];
        for (int i = 0; i < data.Length; i++)
            result[i] = (byte)(data[i] ^ Key);
        return result;
    }
    
    public static string Encrypt(string plaintext)
    {
        var bytes = Encoding.UTF8.GetBytes(plaintext);
        var xored = XorBytes(bytes);
        return Encoding.UTF8.GetString(xored);
    }
    
    public static string Decrypt(string ciphertext)
    {
        var bytes = Encoding.UTF8.GetBytes(ciphertext);
        var xored = XorBytes(bytes);
        return Encoding.UTF8.GetString(xored);
    }
}