/* Shell-only Android11 output capture; no APK, microphone, UI or game changes. */
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.app.Application;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;

public final class AudioPlaybackProbe {
    static void le32(RandomAccessFile f,int n)throws Exception{
        for(int i=0;i<4;i++)f.write(n>>>(8*i));
    }
    static void le16(RandomAccessFile f,int n)throws Exception{f.write(n);f.write(n>>>8);}
    public static void main(String[] args)throws Exception{
        /* app_process has no Application. AudioRecord otherwise attributes
         * shell UID2000 to the nonexistent package "uid:2000" and AppOps denies it. */
        Class<?> threadClass=Class.forName("android.app.ActivityThread");
        if(android.os.Looper.myLooper()==null)android.os.Looper.prepareMainLooper();
        java.lang.reflect.Constructor<?> constructor=threadClass.getDeclaredConstructor();constructor.setAccessible(true);
        Object thread=constructor.newInstance();
        Field current=threadClass.getDeclaredField("sCurrentActivityThread");current.setAccessible(true);current.set(null,thread);
        Application app=new Application();
        Field base=ContextWrapper.class.getDeclaredField("mBase");base.setAccessible(true);
        base.set(app,new ContextWrapper(null){
            @Override public String getPackageName(){return "com.android.shell";}
            @Override public String getOpPackageName(){return "com.android.shell";}
            @Override public ApplicationInfo getApplicationInfo(){
                ApplicationInfo info=new ApplicationInfo();info.packageName="com.android.shell";info.targetSdkVersion=30;return info;
            }
        });
        Field initial=threadClass.getDeclaredField("mInitialApplication");initial.setAccessible(true);initial.set(thread,app);
        int rate=48000,seconds=Integer.parseInt(args[1]);
        int minimum=AudioRecord.getMinBufferSize(rate,AudioFormat.CHANNEL_IN_STEREO,AudioFormat.ENCODING_PCM_16BIT);
        AudioRecord record=new AudioRecord(MediaRecorder.AudioSource.REMOTE_SUBMIX,rate,
            AudioFormat.CHANNEL_IN_STEREO,AudioFormat.ENCODING_PCM_16BIT,Math.max(minimum*4,19200));
        if(record.getState()!=AudioRecord.STATE_INITIALIZED)throw new IllegalStateException("output capture not initialized");
        long count=0,sum=0,begin=System.nanoTime();int peak=0;
        try(RandomAccessFile f=new RandomAccessFile(args[0],"rw")){
            f.setLength(0);f.writeBytes("RIFF");le32(f,0);f.writeBytes("WAVEfmt ");le32(f,16);
            le16(f,1);le16(f,2);le32(f,rate);le32(f,rate*4);le16(f,4);le16(f,16);
            f.writeBytes("data");le32(f,0);
            record.startRecording();begin=System.nanoTime();
            System.out.println("CAPTURE_START_NS="+begin);
            byte[] data=new byte[9600];long end=begin+seconds*1000000000L;
            while(System.nanoTime()<end){
                int n=record.read(data,0,data.length);
                if(n<0)throw new IllegalStateException("AudioRecord.read="+n);
                f.write(data,0,n);count+=n;
                for(int i=0;i+1<n;i+=2){int v=(short)((data[i]&255)|(data[i+1]<<8));sum+=(long)v*v;peak=Math.max(peak,Math.abs(v));}
            }
            record.stop();f.seek(4);le32(f,(int)count+36);f.seek(40);le32(f,(int)count);
        } finally {record.release();}
        System.out.println("CAPTURE_END_NS="+System.nanoTime()+" FRAMES="+count/4+" PEAK="+peak+" RMS="+Math.sqrt((double)sum/(count/2)));
    }
}
