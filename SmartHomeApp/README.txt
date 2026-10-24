ANDROID (Android Studio, Java)
1. New Project > "Empty Views Activity" > Language: Java, Package name: com.example.smarthome (Name: SmartHome)
2. Overwrite/copy these into the project:
   app/src/main/java/com/example/smarthome/MainActivity.java
   app/src/main/res/layout/activity_main.xml
   app/src/main/res/drawable/card_bg.xml
   app/src/main/res/drawable/banner_bg.xml
3. AndroidManifest.xml: add above <application>:
     <uses-permission android:name="android.permission.INTERNET" />
   and inside the <application ...> tag add:
     android:usesCleartextTraffic="true"
4. Run. (Emulator reaches the internet directly; a phone just needs internet.)

iOS (Xcode, SwiftUI)
1. New Project > iOS > App > Interface: SwiftUI, Language: Swift
2. Replace the contents of ContentView.swift with ios/ContentView.swift (iOS 17+ target).
3. Target > Info tab > add "App Transport Security Settings" (NSAppTransportSecurity) > "Allow Arbitrary Loads" = YES
   (needed because the server is plain http on :1880)
4. Run.
