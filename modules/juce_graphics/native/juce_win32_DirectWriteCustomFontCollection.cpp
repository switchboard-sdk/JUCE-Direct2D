
class DirectWriteCustomFontCollectionLoader : public ComBaseClassHelper<IDWriteFontCollectionLoader>
{
private:
    struct FontRawData
    {
        const void* data;
        size_t numBytes;
    };

    struct FontFileStream : public ComBaseClassHelper<IDWriteFontFileStream>
    {
        FontFileStream(FontRawData const& rawData_) :
            rawData(rawData_)
        {
        }
        ~FontFileStream() override = default;

        HRESULT GetFileSize(UINT64* fileSize) override
        {
            DBG("            FontFileStream::GetFileSize");
            *fileSize = rawData.numBytes;
            return S_OK;
        }

        HRESULT GetLastWriteTime(UINT64* lastWriteTime) override
        {
            DBG("            FontFileStream::GetLastWriteTime");
            *lastWriteTime = 0;
            return S_OK;
        }

        HRESULT ReadFileFragment(void const** fragmentStart, UINT64 fileOffset, UINT64 fragmentSize, void** fragmentContext) override
        {
            //DBG("            FontFileStream::ReadFileFragment fileOffset:" << (int64_t)fileOffset << ", fragmentSize:" << (int64_t)fragmentSize);

            if (fileOffset + fragmentSize >= rawData.numBytes)
            {
                *fragmentStart = nullptr;
                *fragmentContext = nullptr;
                return E_INVALIDARG;
            }

            *fragmentStart = addBytesToPointer(rawData.data, fileOffset);
            *fragmentContext = this;
            return S_OK;
        }

        void ReleaseFileFragment(void* /*fragmentContext*/) override
        {
        }

        FontRawData rawData;
    };

    struct FontFileLoader : public ComBaseClassHelper<IDWriteFontFileLoader>
    {
        Array<FontRawData> rawDataArray;

        FontFileLoader()
        {
        }
        ~FontFileLoader() override = default;

        HRESULT CreateStreamFromKey(void const* fontFileReferenceKey, UINT32 fontFileReferenceKeySize, IDWriteFontFileStream** fontFileStream) override
        {
            DBG("         FontFileLoader::CreateStreamFromKey");

            jassert(sizeof(char*) == fontFileReferenceKeySize);
            if (sizeof(char*) == fontFileReferenceKeySize)
            {
                const char* referenceKey = *(const char**)fontFileReferenceKey;
                for (auto const& rawData : rawDataArray)
                {
                    if (referenceKey == rawData.data)
                    {
                        *fontFileStream = new FontFileStream{ rawData };
                        return S_OK;
                    }
                }
            }

            *fontFileStream = nullptr;
            return E_INVALIDARG;
        }

        void addCustomRawFontData(const void* data, size_t dataSize)
        {
            for (auto const& rawData : rawDataArray)
            {
                if (data == rawData.data)
                {
                    return;
                }
            }

            rawDataArray.add({ data, dataSize });
        }
    } fontFileLoader;

    struct FontFileEnumerator : public ComBaseClassHelper<IDWriteFontFileEnumerator>
    {
        FontFileEnumerator(IDWriteFactory* factory_, FontFileLoader& fontFileLoader_, void const* /*collectionKey_*/, UINT32 /*collectionKeySize_*/) :
            factory(factory_),
            fontFileLoader(fontFileLoader_)
        {
        }

        ~FontFileEnumerator() override = default;

        HRESULT GetCurrentFontFile(IDWriteFontFile** fontFile) override
        {
            DBG("   FontFileEnumerator::GetCurrentFontFile rawDataIndex:" << rawDataIndex);
            if (rawDataIndex < 0 || rawDataIndex >= fontFileLoader.rawDataArray.size())
            {
                DBG("      rawDataIndex " << rawDataIndex << " out of range");
                *fontFile = nullptr;
                return E_FAIL;
            }

            auto referenceKey = fontFileLoader.rawDataArray[rawDataIndex].data;
            return factory->CreateCustomFontFileReference(&referenceKey,
                sizeof(referenceKey),
                &fontFileLoader,
                fontFile);
        }

        HRESULT MoveNext(BOOL* hasCurrentFile) override
        {
            ++rawDataIndex;
            *hasCurrentFile = rawDataIndex < fontFileLoader.rawDataArray.size() ? TRUE : FALSE;
            return S_OK;
        }

        IDWriteFactory* factory;
        FontFileLoader& fontFileLoader;
        int rawDataIndex = -1;
    };

public:
    DirectWriteCustomFontCollectionLoader()
    {
    }
    ~DirectWriteCustomFontCollectionLoader() override = default;

    HRESULT CreateEnumeratorFromKey(
        IDWriteFactory* factory,
        void const* collectionKey,
        UINT32 collectionKeySize,
        IDWriteFontFileEnumerator** fontFileEnumerator
    ) override
    {
        *fontFileEnumerator = new FontFileEnumerator{ factory, fontFileLoader, collectionKey, collectionKeySize };
        return S_OK;
    }

    IDWriteFontFileLoader* getFontFileLoader() const
    {
        return (IDWriteFontFileLoader*)&fontFileLoader;
    }

    void addCustomRawFontData(const void* data, size_t dataSize)
    {
        fontFileLoader.addCustomRawFontData(data, dataSize);
    }

#if 0
    Internal(DirectWrite* directWrite_) :
        fontCollectionLoader(fontFileLoader),
        directWrite(directWrite_)
    {
        auto hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, direct2dFactory.resetAndGetPointerAddress());
        if (SUCCEEDED(hr))
        {
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)directWriteFactory.resetAndGetPointerAddress());
            if (SUCCEEDED(hr))
            {
                hr = directWriteFactory->RegisterFontFileLoader(&fontFileLoader);
                if (SUCCEEDED(hr))
                {
                    hr = directWriteFactory->RegisterFontCollectionLoader(&fontCollectionLoader);
                    if (SUCCEEDED(hr))
                    {
                        hr = directWriteFactory->CreateCustomFontCollection(&fontCollectionLoader,
                            &testCollectionKey,
                            sizeof(testCollectionKey),
                            fontCollection.resetAndGetPointerAddress());
                        if (SUCCEEDED(hr))
                        {
                            DBG("Font family count " << (int)fontCollection->GetFontFamilyCount());
                        }
                    }
                }
            }
        }
    }

    ~Internal()
    {
        if (directWriteFactory)
        {
            directWriteFactory->UnregisterFontCollectionLoader(&fontCollectionLoader);
            directWriteFactory->UnregisterFontFileLoader(&fontFileLoader);
        }
    }

    DirectWrite* directWrite;
#endif
};
