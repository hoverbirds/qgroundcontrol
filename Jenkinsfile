pipeline {
  agent any
  stages {
    stage('build') {
      parallel {

        stage('Android Debug') {
          agent {
            docker {
              image 'mavlink/qgc-build-android'
            }
            
          }
          steps {
            sh 'git submodule update --init --recursive'
            sh 'rm -rf build; mkdir build'
            sh 'cd build && qmake -r ../qgroundcontrol.pro CONFIG+=debug CONFIG+=WarningsAsErrorsOn'
            sh 'cd build && make -j2'
          }
        }

        stage('Android Release') {
          agent {
            docker {
              image 'mavlink/qgc-build-android'
            }
            
          }
          steps {
            sh 'git submodule update --init --recursive'
            sh 'rm -rf build; mkdir build'
            sh 'cd build && qmake -r ../qgroundcontrol.pro CONFIG+=installer CONFIG+=WarningsAsErrorsOn'
            sh 'cd build && make -j2'
          }
        }

        stage('Linux Debug') {
          agent {
            docker {
              image 'mavlink/qgc-build-linux'
            }
            
          }
          steps {
            sh 'git submodule update --init --recursive'
            sh 'rm -rf build; mkdir build'
            sh 'cd build && qmake -r ../qgroundcontrol.pro CONFIG+=debug CONFIG+=WarningsAsErrorsOn'
            sh 'cd build && make -j2'
            sh 'cd build && HOME=$WORKSPACE ./debug/qgroundcontrol-start.sh --unittest'
          }
        }

        stage('Linux Release') {
          agent {
            docker {
              image 'mavlink/qgc-build-linux'
            }
            
          }
          steps {
            sh 'git submodule update --init --recursive'
            sh 'rm -rf build; mkdir build'
            sh 'cd build && qmake -r ../qgroundcontrol.pro CONFIG+=installer CONFIG+=WarningsAsErrorsOn'
            sh 'cd build && make -j2'
          }
        }

      }
    }
  }
}