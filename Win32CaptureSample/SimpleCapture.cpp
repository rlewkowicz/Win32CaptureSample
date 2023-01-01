#include "pch.h"
#include "SimpleCapture.h"
#include "unknwn.h"
#include "debugapi.h"
#include <atlstr.h>
#include  <sstream>
std::mutex* p_frame_mutex;
using namespace std;
namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Numerics;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
    using namespace Windows::System;
    using namespace Windows::UI;
    using namespace Windows::UI::Composition;
}

namespace util
{
    using namespace robmikh::common::uwp;
}
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device2{ nullptr };


SimpleCapture::SimpleCapture(winrt::IDirect3DDevice const& device, winrt::GraphicsCaptureItem const& item, winrt::DirectXPixelFormat pixelFormat, winrt::IDirect3DDevice const& device1, winrt::IDirect3DDevice const& device2, std::mutex &frame_mutex, int &id, int& current)
{
    m_item = item;
    m_device = device;
    m_pixelFormat = pixelFormat;
    m_device1 = device1;
    m_device2 = device2;
    p_frame_mutex = &frame_mutex;
    current_thread = &current;

    std::lock_guard<std::mutex> guard(frame_mutex);

    iam = id + 1;

    id += 1;
    
    CString t;

    t.Format(_T("%d\n"), iam);

    OutputDebugStringW(t);


    auto d3dDevice = GetDXGIInterfaceFromObject<ID3D11Device>(m_device);
    d3dDevice->GetImmediateContext(m_d3dContext.put());
    d3dDevice1 = GetDXGIInterfaceFromObject<ID3D11Device>(m_device1);
    d3dDevice1->GetImmediateContext(m_d3dContext1.put());
    cv::directx::ocl::initializeContextFromD3D11Device(d3dDevice1.get());

    m_swapChain = util::CreateDXGISwapChain(d3dDevice, static_cast<uint32_t>(m_item.Size().Width), static_cast<uint32_t>(m_item.Size().Height),
        static_cast<DXGI_FORMAT>(m_pixelFormat), 2);

    // Creating our frame pool with 'Create' instead of 'CreateFreeThreaded'
    // means that the frame pool's FrameArrived event is called on the thread
    // the frame pool was created on. This also means that the creating thread
    // must have a DispatcherQueue. If you use this method, it's best not to do
    // it on the UI thread. 
    m_framePool = winrt::Direct3D11CaptureFramePool::Create(m_device, m_pixelFormat, 2, m_item.Size());
    m_session = m_framePool.CreateCaptureSession(m_item);
    m_lastSize = m_item.Size();
    m_framePool.FrameArrived({ this, &SimpleCapture::OnFrameArrived });
    eCAL::Initialize(NULL, NULL, "frames");
    pub = eCAL::string::CPublisher<std::string>("frames");
    frame_mutex.unlock();
    //cv::namedWindow("enemy");
    //cv::resizeWindow("enemy", 640, 640);
}

void SimpleCapture::StartCapture()
{
    CheckClosed();
    m_session.StartCapture();
}

winrt::ICompositionSurface SimpleCapture::CreateSurface(winrt::Compositor const& compositor)
{
    CheckClosed();
    return util::CreateCompositionSurfaceForSwapChain(compositor, m_swapChain.get());
}

void SimpleCapture::Close()
{
    auto expected = false;
    if (m_closed.compare_exchange_strong(expected, true))
    {
        m_session.Close();
        m_framePool.Close();

        m_swapChain = nullptr;
        m_framePool = nullptr;
        m_session = nullptr;
        m_item = nullptr;
    }
}

void SimpleCapture::ResizeSwapChain()
{
    winrt::check_hresult(m_swapChain->ResizeBuffers(2, static_cast<uint32_t>(m_lastSize.Width), static_cast<uint32_t>(m_lastSize.Height),
        static_cast<DXGI_FORMAT>(m_pixelFormat), 0));
}

bool SimpleCapture::TryResizeSwapChain(winrt::Direct3D11CaptureFrame const& frame)
{
    auto const contentSize = frame.ContentSize();
    if ((contentSize.Width != m_lastSize.Width) ||
        (contentSize.Height != m_lastSize.Height))
    {
        // The thing we have been capturing has changed size, resize the swap chain to match.
        m_lastSize = contentSize;
        ResizeSwapChain();
        return true;
    }
    return false;
}

bool SimpleCapture::TryUpdatePixelFormat()
{
    auto newFormat = m_pixelFormatUpdate.exchange(std::nullopt);
    if (newFormat.has_value())
    {
        auto pixelFormat = newFormat.value();
        if (pixelFormat != m_pixelFormat)
        {
            m_pixelFormat = pixelFormat;
            ResizeSwapChain();
            return true;
        }
    }
    return false;
}

winrt::com_ptr<ID3D11Device> d3dDevice2;
winrt::com_ptr<ID3D11DeviceContext> m_d3dContext2{ nullptr };
stringstream serialize(cv::Mat input)
{
    // We will need to also serialize the width, height, type and size of the matrix
    int width = input.cols;
    int height = input.rows;
    int type = input.type();
    size_t size = input.total() * input.elemSize();

    // Initialize a stringstream and write the data
    stringstream ss;
    ss.write((char*)(&width), sizeof(int));
    ss.write((char*)(&height), sizeof(int));
    ss.write((char*)(&type), sizeof(int));
    ss.write((char*)(&size), sizeof(size_t));

    // Write the whole image data
    ss.write((char*)input.data, size);

    return ss;
}
int SimpleCapture::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&)
{

    frame_count = frame_count + 1;
    auto swapChainResizedToFrame = false;
    auto frame = sender.TryGetNextFrame();
    swapChainResizedToFrame = TryResizeSwapChain(frame);
        
    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    winrt::check_hresult(m_swapChain->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), backBuffer.put_void()));
    auto surfaceTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

    m_d3dContext->CopyResource(backBuffer.get(), surfaceTexture.get());


    DXGI_PRESENT_PARAMETERS presentParameters{};
    m_swapChain->Present1(1, 0, &presentParameters);

    swapChainResizedToFrame = swapChainResizedToFrame || TryUpdatePixelFormat();

    if (swapChainResizedToFrame)
    {
        m_framePool.Recreate(m_device, m_pixelFormat, 2, m_lastSize);
    }

    if (iam != *current_thread){
        frame_count = 0;
        //CString a;

        //a.Format(_T("%d\n"), iam);
        //CString b;

        //b.Format(_T("%d\n"), *current_thread);
        //OutputDebugStringA("waiting\n");

        //OutputDebugStringW(a);
        //OutputDebugStringW(b);

        //return 0;
    }

    D3D11_TEXTURE2D_DESC desc;

    surfaceTexture->GetDesc(&desc);

    D3D11_BOX my_box;
    cv::Mat mat;
    ID3D11Texture2D* myText;

    my_box.front = 0;
    my_box.back = 1;
    my_box.left = 1600;
    my_box.top = 480;
    my_box.right = 2240;
    my_box.bottom = -160;

    desc.Width = 640;
    desc.Height = 640;
    desc.ArraySize = 1;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.MipLevels = 1;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    auto d3dDevice2 = GetDXGIInterfaceFromObject<ID3D11Device>(m_device2);
    d3dDevice2->GetImmediateContext(m_d3dContext2.put());
    d3dDevice2->CreateTexture2D(&desc, NULL, &myText);
    m_d3dContext2->CopySubresourceRegion(myText, D3D11CalcSubresource(0, 0, 1), 0, 0, 0, surfaceTexture.get(), 0, &my_box);
    cv::directx::convertFromD3D11Texture2D(myText, mat);
           
    cv::cvtColor(mat, mat, cv::COLOR_RGBA2RGB);
    /*cv::imshow("enemy", mat);*/
    /*size_t size = mat.total() * mat.elemSize();
    stringstream ss;
    ss.write((char*)mat.data, size);*/
    
    //std::string serialized(mat.data, mat.data + 640 * 640 * 3);
    //auto byte_buffer = builder.CreateVector(mat.data, sizeof(mat.data));
    
    //CString t;

    //t.Format(_T("%d\n"), sizeof(mat.data));

    //OutputDebugStringW(t);
    
    //auto mloc = Dx11::Frame::CreateFrame(builder, byte_buffer);

    //builder.Finish(mloc);
    //OutputDebugString(ss.str());
    cv::Mat1b linear_img(mat.reshape(1));
    string matAsString(linear_img.datastart, linear_img.dataend);
    pub.Send(matAsString, -1);

    //if (frame_count > 1000) {
    //    frame_count = 0;
    //    *current_thread = *current_thread + 1;
    //    m_device.Close();
    //    m_device1.Close();
    //    m_device2.Close();
    //    m_d3dContext2.detach();
    //    m_d3dContext.detach();
    //    m_d3dContext1.detach();

    //    delete this;
    //};

    myText->Release();

    //CString t;

    //t.Format(_T("%d\n"), frame_count);

    //OutputDebugStringW(t);

    return 0;
}
