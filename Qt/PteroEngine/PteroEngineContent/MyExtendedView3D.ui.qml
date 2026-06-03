import QtQuick
import QtQuick3D
import QtQuick3D.Helpers

View3D {
    id: extendedView3D
    environment: sceneEnvironment
    
    ExtendedSceneEnvironment {
        id: sceneEnvironment
        antialiasingMode: SceneEnvironment.MSAA
        antialiasingQuality: SceneEnvironment.High
    }
    
    Node {
        id: scene
        DirectionalLight {
            id: directionalLight
        }
        
        PerspectiveCamera {
            id: sceneCamera
            z: 350
        }
        
        Model {
            id: cubeModel
            eulerRotation.y: 45
            eulerRotation.x: 30
            materials: defaultMaterial
            source: "#Cube"
        }
    }
    
    Item {
        id: __materialLibrary__
        
        PrincipledMaterial {
            id: defaultMaterial
            objectName: "Default Material"
            baseColor: "#4aee45"
        }
    }
}
